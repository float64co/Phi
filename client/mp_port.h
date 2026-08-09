#pragma once
#include <stddef.h>

/* Phi's own thin API over the embedded MicroPython interpreter (see
 * mp_port.c) -- wraps the exact init/exec pattern client/mp_test_main.c
 * and client/mp_stress_test_main.c already proved works, so main.c and
 * console.c don't need to touch MicroPython's own headers
 * (port/micropython_embed.h, py/runtime.h, nlr_buf_t, ...) directly. */

/* Initializes the embedded interpreter -- call exactly once, from as
 * close to main()'s own top-level scope as possible, passing the address
 * of a local variable declared THERE (not inside this function, and not
 * inside a helper called deeper in) as stack_top: MicroPython's GC does a
 * conservative scan of the C stack between the CURRENT stack pointer and
 * this recorded boundary every collection, so it needs to be a real,
 * durable point at (or above) every stack depth the interpreter could
 * ever be called from later. For native/win32 this is trivially safe --
 * phi_platform_set_main_loop() blocks in a real loop inside main()'s own
 * still-active frame for the rest of the program's life. For wasm,
 * emscripten_set_main_loop's simulate_infinite_loop mode is specifically
 * designed so a callback registered this way keeps seeing a consistent
 * per-frame stack depth close to the original call site, which is why
 * "capture stack_top once before the loop, from main() itself" is the
 * standard Emscripten pattern for exactly this kind of conservative GC --
 * not something invented here. */
void phi_mp_init(void *stack_top);

/* Executes one block of Python source through mp_embed_exec_str (already
 * exception-safe on its own -- an uncaught Python exception gets printed
 * as a traceback via mp_plat_print rather than crashing/hanging the
 * process, see micropython_embed/port/embed_util.c's own nlr_push), after
 * first resetting the shared output-capture buffer that mpconfigport.h's
 * MP_PLAT_PRINT_STRN override routes every print()/traceback through
 * (see mp_port.c). Returns a newly malloc'd, NUL-terminated string
 * containing everything printed during the call (real print() output and
 * any exception traceback both flow through the same path, so a raised
 * exception's text shows up here too, not as a separate error channel) --
 * caller frees it. Never returns NULL; an empty string means the code
 * printed nothing and didn't raise. */
char *phi_mp_exec(const char *code);
