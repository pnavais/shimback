#ifndef SHIMBACK_TUI_H
#define SHIMBACK_TUI_H

#include <stdbool.h>

/* Minimal raw-terminal primitives for a small interactive prompt flow (see
 * commands/add_wizard.c). Not a general TUI framework -- no windows, no
 * alternate screen buffer, just enough to read one logical keypress at a
 * time and redraw a full frame. */

typedef enum {
    TUI_KEY_NONE = 0,
    TUI_KEY_CHAR,      /* a printable character, value in .ch */
    TUI_KEY_ENTER,
    TUI_KEY_BACKSPACE,
    TUI_KEY_TAB,
    TUI_KEY_ABORT,     /* a bare Esc, or Ctrl-C -- both mean "abort" */
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
} TuiKeyType;

typedef struct {
    TuiKeyType type;
    char ch; /* valid only when type == TUI_KEY_CHAR */
} TuiKey;

/* True if both stdin and stdout are a real terminal -- callers must check
 * this before attempting any interactive flow at all. */
bool tui_supported(void);

/* Switches stdin into cbreak-like input (no line buffering, no local echo,
 * no signal-generating control chars -- Ctrl-C is read back as a plain byte
 * via tui_read_key() instead of killing the process) while leaving output
 * processing alone, so ordinary printf/\n output still behaves normally.
 * Registers an atexit safety net that restores the original terminal state
 * even if the process exits through a path that skips tui_raw_mode_exit().
 * Returns false (leaving the terminal untouched) on an actual termios
 * failure. */
bool tui_raw_mode_enter(void);

/* Restores the terminal state saved by tui_raw_mode_enter(). Safe to call
 * even if raw mode was never entered (a no-op then). */
void tui_raw_mode_exit(void);

/* Blocking read of the next logical keypress. Decodes "ESC [ A/B/C/D" as
 * arrow keys; a bare Esc (nothing else follows within ~50ms) and Ctrl-C
 * both yield TUI_KEY_ABORT; 0x7f/0x08 yield TUI_KEY_BACKSPACE; '\r'/'\n'
 * yield TUI_KEY_ENTER; '\t' yields TUI_KEY_TAB; any other unrecognized
 * escape sequence is swallowed and yields TUI_KEY_NONE (caller should just
 * redraw and keep reading). */
TuiKey tui_read_key(void);

/* Clears the screen and homes the cursor, ready for a fresh frame. */
void tui_clear_screen(void);

#endif /* SHIMBACK_TUI_H */
