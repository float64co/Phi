/* Phi's own MicroPython "port" glue — NOT part of the generated
 * client/micropython_embed/ tree (that's regenerated from upstream, see
 * client/mpconfigport.h's header comment; this file is hand-written and
 * stays put across regenerations). Two unrelated jobs live here:
 *
 * 1. mp_lexer_new_from_file below: this embedding has no real filesystem
 *    -- every script Phi runs arrives as in-memory text
 *    (mp_embed_exec_str), never loaded via a file path. Config in
 *    mpconfigport.h turns off the code paths that assume file-backed
 *    import/open/stdio exist, EXCEPT exec()/eval()'s file-path overload
 *    (builtinevex.c's eval_exec_helper), which still references
 *    mp_lexer_new_from_file unconditionally regardless of that config.
 *    Phi never calls exec()/eval() with a filename argument, so this
 *    only needs to link, not actually succeed — it raises OSError if
 *    ever reached.
 *
 * 2. phi_mp_init/phi_mp_exec/phi_mp_capture_output further down (see
 *    mp_port.h): the real interpreter lifecycle + a Console-facing exec
 *    call that captures print()/traceback output instead of letting it
 *    go to the process's real stdout, invisible from inside the game
 *    window -- this is Phase 1's "Console panel becomes a real Python
 *    REPL" piece (see phi.md), NOT Phase 5's phi.emit/ctx.prop/
 *    @phi.panel/@phi.node decorator API surface, which stays out of
 *    scope here on purpose. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "py/lexer.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "port/micropython_embed.h"
#include "mp_port.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    mp_raise_OSError(ENOENT);
}

/* ---- Phi's own init/exec/output-capture glue (see mp_port.h) ---- */

/* Same 64KB size client/mp_test_main.c/mp_stress_test_main.c already
 * proved sufficient for the @phi.panel/@phi.node decorator-pattern
 * validation scripts -- this pass's Console-typed one-liners are no
 * heavier than those. */
static char s_mp_heap[64 * 1024];

static char  *s_capture_buf = NULL;
static size_t s_capture_len = 0, s_capture_cap = 0;

/* Routed here by mpconfigport.h's MP_PLAT_PRINT_STRN override -- this is
 * every byte print()/an exception traceback would otherwise send to the
 * real process stdout (mphalport.c's default mp_hal_stdout_tx_strn_cooked,
 * invisible from inside the game window), captured instead so the Console
 * panel can show it. */
void phi_mp_capture_output(const char *str, size_t len) {
    if (s_capture_len + len + 1 > s_capture_cap) {
        size_t new_cap = s_capture_cap ? s_capture_cap * 2 : 256;
        while (new_cap < s_capture_len + len + 1) new_cap *= 2;
        s_capture_buf = (char *)realloc(s_capture_buf, new_cap);
        s_capture_cap = new_cap;
    }
    memcpy(s_capture_buf + s_capture_len, str, len);
    s_capture_len += len;
    s_capture_buf[s_capture_len] = 0;
}

void phi_mp_init(void *stack_top) {
    mp_embed_init(&s_mp_heap[0], sizeof(s_mp_heap), stack_top);
    /* mp_embed_init only calls the deprecated mp_stack_set_top(), which
     * never sets stack_limit -- py/cstack.c's mp_cstack_check() (what's
     * actually compiled in at this ROM level) reads that same state and
     * treats the default-zero limit as "already over budget", raising a
     * false RecursionError on the very first check, caught nowhere, which
     * hangs the process in nlr_jump_fail's infinite loop. Root-caused via
     * gdb backtrace during this session's mp_test_main.c development
     * (see its own comment) -- the exact same workaround applies here,
     * for the exact same reason. mp_stack_set_limit() is the old (but
     * still functional) stackctrl.c API and writes the same
     * MP_STATE_THREAD(stack_limit) cstack.c reads. */
    mp_stack_set_limit(32 * 1024);
}

char *phi_mp_exec(const char *code) {
    s_capture_len = 0;
    if (s_capture_buf) s_capture_buf[0] = 0;
    mp_embed_exec_str(code);
    return strdup(s_capture_buf ? s_capture_buf : "");
}
