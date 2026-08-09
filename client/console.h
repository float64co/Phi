#pragma once
#include "input.h"

#define CONSOLE_LOG_LINES  12
#define CONSOLE_LINE_LEN   128
#define CONSOLE_INPUT_LEN  96
#define CONSOLE_HISTORY    16

/* Python panel — a real REPL over the embedded MicroPython interpreter
 * (mp_port.h), acting as an always-focused text input: keyboard input
 * flows here every frame, apart from the few reserved shortcuts input.c
 * keeps (function keys only, so they can't collide with typing). There is
 * no open/close focus toggle — Qek's backquote-toggled modal drop-down
 * console paradigm is gone, along with every one of its dev-commands
 * (removed rather than ported forward; anything typed here just runs as
 * Python, full stop). That's also why this header depends only on
 * input.h: console.c has no reason to touch any game/editor state.
 * Text fields are exposed directly (not opaque) so ui.c's panel drawing
 * can read them straight out of the struct. */
typedef struct {
    char input[CONSOLE_INPUT_LEN];
    int  input_len;

    char log[CONSOLE_LOG_LINES][CONSOLE_LINE_LEN];
    int  log_count;

    char history[CONSOLE_HISTORY][CONSOLE_INPUT_LEN];
    int  history_count;
    int  history_pos;   /* -1 = not browsing history */
} PyConsoleState;

/* Registers cs as the target for pyconsole_append() (mirrors input.c's
 * single-instance s_inp pattern) — there's only ever one console. */
void pyconsole_init(PyConsoleState *cs);

void pyconsole_update(PyConsoleState *cs, InputState *inp);

/* Appends a line to the console log. Called by net.c on PKT_CONSOLE_MSG,
 * and usable for any other "print to console" need. */
void pyconsole_append(const char *line);
