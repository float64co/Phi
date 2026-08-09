#pragma once
#include "input.h"

#define CONSOLE_LOG_LINES  12
#define CONSOLE_LINE_LEN   128
#define CONSOLE_INPUT_LEN  96
#define CONSOLE_HISTORY    16

/* Drop-down Python console (` to toggle) — a real REPL over the embedded
 * MicroPython interpreter (mp_port.h), not a bespoke dev-command shell.
 * We're in Phi now, not Qek: every one of Qek's old console commands
 * (pos/tp/grid/mat/noclip/god/hp/give/speed/gravity/fov/sensitivity/name/
 * players/kill/save/load/newmap/maps/addbot/delbot/bind/unbind, plus this
 * session's own matcolor/matmetal/matrough/matemit stopgap) has been
 * removed rather than ported forward — anything typed here just runs as
 * Python, full stop. That's also why this header no longer depends on
 * editor.h/net.h/physics.h/renderer.h/meshobject.h: console.c has no
 * reason to touch any of that game/editor state anymore. Text fields are
 * exposed directly (not opaque) so main.c can hand them straight to a DOM
 * overlay the same way it already does for the HUD via EM_ASM. */
typedef struct {
    int  open;
    char input[CONSOLE_INPUT_LEN];
    int  input_len;

    char log[CONSOLE_LOG_LINES][CONSOLE_LINE_LEN];
    int  log_count;

    char history[CONSOLE_HISTORY][CONSOLE_INPUT_LEN];
    int  history_count;
    int  history_pos;   /* -1 = not browsing history */
} ConsoleState;

/* Registers cs as the target for console_append() (mirrors input.c's
 * single-instance s_inp pattern) — there's only ever one console. */
void console_init(ConsoleState *cs);

void console_update(ConsoleState *cs, InputState *inp);

/* Appends a line to the console log. Called by net.c on PKT_CONSOLE_MSG,
 * and usable for any other "print to console" need. */
void console_append(const char *line);
