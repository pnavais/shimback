#include "tui.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved_termios;
static bool g_have_saved_termios = false;
static bool g_raw_mode_active = false;

bool tui_supported(void) {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static void restore_at_exit(void) {
    tui_raw_mode_exit();
}

bool tui_raw_mode_enter(void) {
    if (g_raw_mode_active) {
        return true;
    }
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &raw) != 0) {
        return false;
    }
    if (!g_have_saved_termios) {
        g_saved_termios = raw;
        g_have_saved_termios = true;
        atexit(restore_at_exit);
    }
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(unsigned)(IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return false;
    }
    g_raw_mode_active = true;
    return true;
}

void tui_raw_mode_exit(void) {
    if (!g_raw_mode_active || !g_have_saved_termios) {
        return;
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    g_raw_mode_active = false;
}

/* Waits up to `ms` milliseconds for another byte to be ready on stdin --
 * used only to tell a bare Esc apart from the start of "ESC [ <letter>". */
static bool byte_ready_within(int ms) {
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    return poll(&pfd, 1, ms) > 0;
}

static bool read_one_byte(char *out) {
    ssize_t n = read(STDIN_FILENO, out, 1);
    return n == 1;
}

TuiKey tui_read_key(void) {
    char c;
    if (!read_one_byte(&c)) {
        return (TuiKey){.type = TUI_KEY_ABORT, .ch = 0};
    }

    if (c == '\r' || c == '\n') {
        return (TuiKey){.type = TUI_KEY_ENTER, .ch = 0};
    }
    if (c == 0x7f || c == 0x08) {
        return (TuiKey){.type = TUI_KEY_BACKSPACE, .ch = 0};
    }
    if (c == 0x03) { /* Ctrl-C, read as a plain byte since ISIG is off */
        return (TuiKey){.type = TUI_KEY_ABORT, .ch = 0};
    }
    if (c == 0x1b) { /* Esc, or the start of an escape sequence */
        if (!byte_ready_within(50)) {
            return (TuiKey){.type = TUI_KEY_ABORT, .ch = 0};
        }
        char c2;
        if (!read_one_byte(&c2) || c2 != '[') {
            return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
        }
        if (!byte_ready_within(50)) {
            return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
        }
        char c3;
        if (!read_one_byte(&c3)) {
            return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
        }
        switch (c3) {
            case 'A': return (TuiKey){.type = TUI_KEY_UP, .ch = 0};
            case 'B': return (TuiKey){.type = TUI_KEY_DOWN, .ch = 0};
            case 'C': return (TuiKey){.type = TUI_KEY_RIGHT, .ch = 0};
            case 'D': return (TuiKey){.type = TUI_KEY_LEFT, .ch = 0};
            default: return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
        }
    }
    if (c >= 0x20 && c <= 0x7e) {
        return (TuiKey){.type = TUI_KEY_CHAR, .ch = c};
    }
    return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
}

void tui_clear_screen(void) {
    fputs("\x1b[H\x1b[2J", stdout);
    fflush(stdout);
}
