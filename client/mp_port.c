/* Phi's own MicroPython "port" glue — NOT part of the generated
 * client/micropython_embed/ tree (that's regenerated from upstream, see
 * client/mpconfigport.h's header comment; this file is hand-written and
 * stays put across regenerations).
 *
 * This embedding has no real filesystem: every script Phi runs arrives as
 * in-memory text (mp_embed_exec_str), never loaded via a file path. Config
 * in mpconfigport.h turns off the code paths that assume file-backed
 * import/open/stdio exist, EXCEPT exec()/eval()'s file-path overload
 * (builtinevex.c's eval_exec_helper), which still references
 * mp_lexer_new_from_file unconditionally regardless of that config. Phi
 * never calls exec()/eval() with a filename argument, so this only needs
 * to link, not actually succeed — it raises OSError if ever reached. */
#include <errno.h>
#include "py/lexer.h"
#include "py/runtime.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    mp_raise_OSError(ENOENT);
}
