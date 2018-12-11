#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifndef DEBUG
#ifndef RELEASE
#define DEBUG 1
#else
#define DEBUG 0
#endif
#endif

#define COLOR_RESET  "\e[m"
#define COLOR_BOLD   "\e[1m"
#define COLOR_INVERT "\e[7m"

#define SHORT_FLAGS "aBcdFhx"
#define MSG_USAGE   "Usage: %s [-" SHORT_FLAGS "] [<directory>]"
#define MSG_INVALID MSG_USAGE "\nTry '%s -h' for more information.\n"
#define MSG_HELP    MSG_USAGE "\nInteractive exploration of directories on the command line.\n" \
                    "\nFlags:\n" \
                    "  -a\tShow files starting with . (hidden by default)\n" \
                    "  -B\tDon't output color.\n" \
                    "  -c\tClear listing on exit.\n" \
                    "  -d\tPrint current directory path before listing.\n" \
                    "  -F\tAppend ls style indicators to the end of entries.\n" \
                    "  -h\tPrint this message and exit.\n" \
                    "  -x\tPrint unprintable characters as hex.  Carriage return would be /0D/.\n" \
                    "\nKeys:\n" \
                    "   E\tEdit selected entry.\n" \
                    "   O\tOpen selected entry.\n" \
                    "   X\tExecute selected entry.\n" \
                    "   Q\tQuit.\n" \
                    "   K|Up           Go up a directory.\n" \
                    "   J|Down|Enter   Open selected directory.\n" \
                    "   H|Left         Move selection left.\n" \
                    "   L|Right        Move selection right.\n"
#define MSG_CANT_SCAN "/could not scan/"
#define MSG_EMPTY     "/empty/"

#define ENTRY_DELIM "  "

#define OPEN_IN_PROCESS 0
#define OPEN_WITH_FORK  1

static char * d_current_name = NULL;
static int d_current_len = 0; // Strlen of current dir name.
static int d_length = 0;      // Number of entries in current dir.

#define SELECTED_MIN 0
#define SELECTED_MAX (d_length - 1)
static int selected = SELECTED_MIN;
#define SELECTED_MAXLEN 256
static char selected_name[SELECTED_MAXLEN];

static struct termios tcattr_old;
static struct termios tcattr_raw;

static int last_overflow_count = 0; // Number of lines overflowed by last display.

static char cfg_show_dotfiles = 0; // (-a) If set, files starting with . will be shown.
static char cfg_color         = 1; // (-B) If set, color output.  -B unsets this.
static char cfg_clear_trace   = 0; // (-c) If set, clear displayed text on exit.
static char cfg_show_dir      = 0; // (-d) If set, print current dir before listing.
static char cfg_indicate      = 0; // (-F) If set, append indicators to entries.
static char cfg_print_hex     = 0; // (-x) If set, print unprintable characters as hex.

// TEMP:
#define CHECKBAD(val, err, msg, ...) if (val) { putchar('\n'); fprintf(stderr, msg, __VA_ARGS__); exit(err); }

static void restore_tcattr() {
    printf("\e[?25h"); // Show cursor.
    if (cfg_clear_trace) printf("\e[u\e[0J\e[2K"); // Clear last display, see display().
    else for (int l = 0; l <= last_overflow_count; ++l) putchar('\n');

    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_old);
}

// If to, the current directory string being appended, is NULL,
// exact space is allocated and d_current_name is copied.
// Returns to, whether that be the original or a result of the above condition.
static char * append_to_cd(char * to, char * suffix) {
    size_t suffix_len = strlen(suffix);
    size_t to_len = d_current_len + suffix_len + 2;

    CHECKBAD(to_len > PATH_MAX, 1, "%s/%s is too long of a path!", to, suffix);
    if (to == NULL) to = malloc(sizeof(*d_current_name) * to_len);
    CHECKBAD(to == NULL, 1, "Out of memory!%c", 0);

    memcpy(to, d_current_name, sizeof(*d_current_name) * d_current_len);
    to[d_current_len] = '/';
    memcpy(to + d_current_len + 1, suffix, sizeof(*d_current_name) * suffix_len);
    to[d_current_len + 1 + suffix_len] = 0;

    return to;
}

static void cd(char * to) {
    if (d_current_name == NULL) {
        d_current_name = malloc(sizeof(*d_current_name) * PATH_MAX);
        realpath(to, d_current_name);
    } else if (to[0] == '/') {
        realpath(to, d_current_name);
    } else {
        char * new = append_to_cd(NULL, to);
        realpath(new, d_current_name);
        free(new);
    }

    d_current_len = strlen(d_current_name);
    d_length = 0;
    selected = SELECTED_MIN;
}

static void get_entry_type(struct dirent * ent, const char ** color, char * indicator) {
    static const char * colors[] = {
        0,          // DT_UNKNOWN
        "\e[33m",   // DT_FIFO
        "\e[33;1m", // DT_CHR
        0,
        "\e[34;1m", // DT_DIR
        0,
        "\e[33;1m", // DT_BLK
        0,
        0,          // DT_REG
        0,
        "\e[36;1m", // DT_LNK
        0,
        "\e[35;1m", // DT_SOCK
    };
    static const char indicators[] = "\0|\0\0/\0\0\0\0\0@\0=";

    if (ent->d_type > 0 && ent->d_type <= DT_SOCK
            && (colors[ent->d_type] || indicators[ent->d_type])) {
        *color     = colors[ent->d_type];
        *indicator = indicators[ent->d_type];
    } else {
        // d_type couldn't tell us anything, so check if executable.
        char * ent_path = append_to_cd(NULL, ent->d_name);
        if (access(ent_path, X_OK) == 0) {
            *color     = "\e[32;1m";
            *indicator = '*';
        } else {
            *color     = 0;
            *indicator = 0;
        }
        free(ent_path);
    }
}

static int display_filter(const struct dirent * ent) {
    if (ent->d_name[0] == '.') {
        if (!cfg_show_dotfiles) return 0;
        else if (ent->d_name[1] == 0) return 0; // Don't show "."
        else if (ent->d_name[1] == '.' && ent->d_name[2] == 0) return 0; // Don't show ".."
    }
    return 1;
}

// NOTE: This will eat everything in stdin.
static void get_cursor_pos(int * row, int * col) {
    int ahead = 0;
    char c;

    *row = 0;
    *col = 0;

    // Attempt to clear out stdin.
    // CLEANUP: Is read with a NULL buffer really allowed?
    ioctl(STDIN_FILENO, FIONREAD, &ahead);
    read(STDIN_FILENO, NULL, ahead);

    // Request cursor position and scan for the response "\e[%d;%dR".
    // If we read in something that started out correct and became malformed,
    // it isn't the cursor position response so start over.
    printf("\e[6n");
scan_for_esc:
    while (getchar() != 0x1B);               // Scan for escape.
    if (getchar() != '[') goto scan_for_esc; // Scan for [
    while (1) {                              // Scan for %d;
        c = getchar();
        if (c == ';') break;
        else if (c < '0' || c > '9') goto scan_for_esc;
        *row = (*row * 10) + (c - '0');
    }
    while (1) {                              // Scan for %dR
        c = getchar();
        if (c == 'R') break;
        else if (c < '0' || c > '9') goto scan_for_esc;
        *col = (*col * 10) + (c - '0');
    }
}

static void display() {
    struct winsize termsize;
    int print_count = 0;
    int newline_count = 0;
    int row_before, col_before;
    int row_after,  col_after;
    DIR * d_current = NULL;
    struct dirent ** d_children;
    struct dirent * d_child;
    int d_child_len;
    const char * d_child_color;
    char d_child_indicator;

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &termsize);

    d_length = scandir(d_current_name, &d_children, display_filter, alphasort);
    if (d_length == -1) {
        selected_name[0] = 0;
    }

    d_current = opendir(d_current_name);
    CHECKBAD(d_current == NULL, 1, "Could not open %s", d_current_name);

    // Validate selection index.

    if (d_length < 1) selected = 0;
    else if (selected < SELECTED_MIN) selected = SELECTED_MAX;
    else if (selected > SELECTED_MAX) selected = SELECTED_MIN;

    // Return to start of last display and erase previous.
    // 0J erases below cursor, 2K erases to the right.

    printf("\e[u\e[0J\e[2K");
    get_cursor_pos(&row_before, &col_before);

    // If enabled, print current directory.

    if (cfg_show_dir) {
        print_count += printf(COLOR_BOLD COLOR_INVERT "%s" COLOR_RESET ": ", d_current_name);
    }

    // Now we can print the names of each entry.

    printf(COLOR_RESET);

    if (d_length < 0) {
        // The directory couldn't be opened.  Say so.
        print_count += printf(MSG_CANT_SCAN COLOR_RESET " ");
    } else if (d_length == 0) {
        // The directory is empty.  Say so.
        print_count += printf(MSG_EMPTY COLOR_RESET " ");
    }

    for (int i = 0; i < d_length; ++i) {
        d_child = d_children[i];
        d_child_len = strlen(d_child->d_name);
        get_entry_type(d_child, &d_child_color, &d_child_indicator);

        if (i == selected) {
            // Set the selection as valid if it is a directory.
            // Copy the name into the selected_name buffer.

            memcpy(selected_name, d_child->d_name, sizeof(*selected_name) * SELECTED_MAXLEN);
            printf(COLOR_INVERT);
        }

        // Time to print this entry.

        // If enabled, print the corresponding color for the type.
        if (cfg_color && d_child_color) printf("%s", d_child_color);

        // If this entry would line wrap, print a newline.
        // +1 for indicator.
        if (print_count + d_child_len + strlen(ENTRY_DELIM) + 1 >= termsize.ws_col) {
            putchar('\n');
            ++newline_count;
            print_count = 0;
        }

        // Print the name of the entry.
        for (unsigned char * c = (unsigned char *)d_child->d_name; *c; ++c) {
            // This character is printable if
            // it is above control characters and not DEL.
            if (*c > 0x1F && *c != 0x7F) {
                putchar(*c);
                ++print_count;
            } else if (cfg_print_hex) {
                print_count += printf("/%02X/", (unsigned char)*c);
            }
        }

        printf(COLOR_RESET);

        // If enabled, print the corresponding indicator for the type.
        if (cfg_indicate && d_child_indicator) {
            putchar(d_child_indicator);
            ++print_count;
        }

        print_count += printf(ENTRY_DELIM);

        free(d_child);
    }

    if (d_children) free(d_children);
    closedir(d_current);

    // If the lines overflowed does not match the difference in cursor height,
    // the terminal scrolled and we need to adjust the saved position.

    get_cursor_pos(&row_after, &col_after);
    print_count = print_count + newline_count * termsize.ws_col;
    last_overflow_count = print_count / termsize.ws_col;
    if (last_overflow_count) {
        // Move cursor up to adjust for overflow and save it.
        printf("\e[%d;%df\e[s", row_after - last_overflow_count, col_before);
    }
}

static int open_selection(char * opener, int do_fork) {
    char * argv[3] = {0};
    char * selected_path;
    pid_t pid;

    selected_path = append_to_cd(NULL, selected_name);

    // If opener is null, then we are executing the selection, not "opening" it.
    if (opener == NULL) {
        opener  = selected_path;
        argv[0] = selected_path;
    } else {
        argv[0] = opener;
        argv[1] = selected_path;
    }

    if (do_fork) {
        pid = fork();

        if (pid > 0) {
            return 0;
        } else if (pid == 0) {
            execv(opener, argv);
            CHECKBAD(1, 1, "%s failed to execute", opener);
        } else {
            CHECKBAD(1, 1, "Could not start process for %s", opener);
        }
    } else {
        restore_tcattr(); // atexit won't call this since we are overwriting this process.
        if (cfg_clear_trace) putchar('\n'); // execv might stdout buffered before clear happens.
        execv(opener, argv);
        CHECKBAD(1, 1, "%s failed to execute", opener);
    }
}

int main(int argc, char ** argv) {
    int flag;

    setlocale(LC_ALL, "");

    while ((flag = getopt(argc, argv, SHORT_FLAGS)) != -1) { switch(flag) {
    case 'a': cfg_show_dotfiles = 1; break;
    case 'B': cfg_color         = 0; break;
    case 'c': cfg_clear_trace   = 1; break;
    case 'd': cfg_show_dir      = 1; break;
    case 'F': cfg_indicate      = 1; break;
    case 'x': cfg_print_hex     = 1; break;
    case 'h': printf(MSG_HELP, argv[0]); return 0;
    case '?': fprintf(stderr, MSG_INVALID, argv[0], argv[0]); return 1;
    default: abort();
    }}

    // If there is a remaining argument, it is the directory to start in.
    if (optind < argc) cd(argv[optind]);
    else               cd(".");

    // Create raw terminal mode to stop stdin buffer from breaking key press detection.
    // http://pubs.opengroup.org/onlinepubs/000095399/basedefs/termios.h.html#tag_13_74_03_06
    tcgetattr(STDIN_FILENO, &tcattr_old);
    atexit(restore_tcattr); // Restore old mode when we're done.
    memcpy(&tcattr_raw, &tcattr_old, sizeof(struct termios));
    tcattr_raw.c_cc[VMIN]  = 0;
    tcattr_raw.c_cc[VTIME] = 0;
    tcattr_raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &tcattr_raw);
#if DEBUG
    printf("Dev Build %s %s\e[K\n", __DATE__, __TIME__);
#endif
    printf("\e[?25l\e[s"); // Hide cursor and save cursor location.

    display();
redo:
    switch (getchar()) {
    default: goto redo;
    case 'E': case 'e': // Edit
         return open_selection("/usr/bin/vim", OPEN_IN_PROCESS);
    case 'O': case 'o': // Open
         return open_selection("/usr/bin/xdg-open", OPEN_WITH_FORK);
    case 'X': case 'x': // eXecute
         return open_selection(NULL, OPEN_IN_PROCESS);
    case 'K': case 'k': // Select Parent
         cd(".."); break;
    case 'J': case 'j': // Select
    case '\n':
         cd(selected_name); break;
    case 'H': case 'h': // Left
         --selected; break;
    case 'L': case 'l': // Right
         ++selected; break;
    case 'Q': case 'q': // Quit
         return 0;
    case 0x1B: // ESC
         if (getchar() != '[') return 0; // If just escape key, quit.

         // Arrow key escape codes have to be handled seperately than
         // the above handling of ASCII keys.
         switch (getchar()) {
         case 'A': // Select Parent (Up)
             cd(".."); break;
         case 'B': // Select (Down)
             cd(selected_name); break;
         case 'D': // Left
             --selected; break;
         case 'C': // Right
             ++selected; break;
         }
         break;
    }

    display();
    goto redo;
}
