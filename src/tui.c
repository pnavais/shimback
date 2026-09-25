#include "tui.h"

#include <stdio.h>

#include "platform/platform.h"

bool tui_supported(void) {
    return plat_isatty_stdin() && plat_isatty_stdout();
}

bool tui_raw_mode_enter(void) {
    return plat_tty_raw_enter();
}

void tui_raw_mode_exit(void) {
    plat_tty_raw_exit();
}

TuiKey tui_read_key(void) {
    char c;
    if (!plat_read_stdin_byte(&c)) {
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
        if (!plat_stdin_byte_ready(50)) {
            return (TuiKey){.type = TUI_KEY_ABORT, .ch = 0};
        }
        char c2;
        if (!plat_read_stdin_byte(&c2) || c2 != '[') {
            return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
        }
        if (!plat_stdin_byte_ready(50)) {
            return (TuiKey){.type = TUI_KEY_NONE, .ch = 0};
        }
        char c3;
        if (!plat_read_stdin_byte(&c3)) {
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
