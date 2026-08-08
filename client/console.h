#pragma once
#include "editor.h"
#include "input.h"
#include "net.h"
#include "physics.h"
#include "renderer.h"

#define CONSOLE_LOG_LINES  12
#define CONSOLE_LINE_LEN   128
#define CONSOLE_INPUT_LEN  96
#define CONSOLE_HISTORY    16
#define CONSOLE_MAX_BINDS  32

/* Source-engine-style drop-down command console (` to toggle). Text fields
 * are exposed directly (not opaque) so main.c can hand them straight to the
 * DOM overlay the same way it already does for the HUD via EM_ASM. */
typedef struct {
    int  open;
    char input[CONSOLE_INPUT_LEN];
    int  input_len;

    char log[CONSOLE_LOG_LINES][CONSOLE_LINE_LEN];
    int  log_count;

    char history[CONSOLE_HISTORY][CONSOLE_INPUT_LEN];
    int  history_count;
    int  history_pos;   /* -1 = not browsing history */

    /* 'bind'/'unbind': key code (e.g. "KeyG") -> command string, matched
     * against InputState.pressed_codes each frame while the console is
     * closed. Parallel arrays, bind_keys[i] <-> bind_cmds[i]. */
    char bind_keys[CONSOLE_MAX_BINDS][KEY_CODE_LEN];
    char bind_cmds[CONSOLE_MAX_BINDS][CONSOLE_INPUT_LEN];
    int  bind_count;
} ConsoleState;

/* Registers cs as the target for console_append() (mirrors input.c's
 * single-instance s_inp pattern) — there's only ever one console. */
void console_init(ConsoleState *cs);

void console_update(ConsoleState *cs, InputState *inp, EditorState *ed,
                    NetState *ns, GameState *gs, Renderer *r, Player *local);

/* Appends a line to the console log. Called by net.c on PKT_CONSOLE_MSG,
 * and usable for any other "print to console" need. */
void console_append(const char *line);
