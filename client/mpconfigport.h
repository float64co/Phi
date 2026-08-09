/* MicroPython embed-port configuration for Phi (Phase 5 first slice).
 *
 * client/micropython_embed/ is generated output, not hand-written source —
 * produced by running `make -f micropython_embed.mk` from MicroPython's own
 * ports/embed against this file, per MicroPython's documented embedding
 * workflow (examples/embedding/ in the upstream repo). Regenerate it with a
 * fresh checkout of https://github.com/micropython/micropython (MIT,
 * pre-approved per phi.md's Hard Architectural Decisions) rather than
 * hand-editing the generated tree if this config changes.
 *
 * FULL_FEATURES for now, not tuned for footprint yet — this slice is about
 * proving the @phi.panel/@phi.node decorator patterns actually work in real
 * MicroPython (see phi.md's Phase 1/Phase 6 CRITICAL WARNING), which needs
 * every relevant language feature turned ON to get an honest answer. Cutting
 * back to MINIMUM/CORE_FEATURES for the ~200-400KB compiled footprint phi.md
 * targets is follow-up work once the API shape this validates is locked in —
 * doing it now would risk a false negative (a pattern reads as "MicroPython
 * can't do this" when actually "this ROM level doesn't include it").
 *
 * MICROPY_PY_BUILTINS_INPUT off: pulls in shared/readline/readline.h for an
 * interactive-terminal input() builtin this embedding has no host for — Phi
 * drives MicroPython from C by exec'ing script text and calling functions
 * directly, never through an interactive REPL/stdin loop.
 *
 * MICROPY_PY_SYS_PLATFORM defined directly: normally a port-specific string
 * (e.g. "linux", "esp32"); MicroPython's default config has no fallback for
 * it, it just expects every port to set one.
 */
#include <stddef.h>   /* size_t, for phi_mp_capture_output's declaration below */
#include <port/mpconfigport_common.h>
#define MICROPY_CONFIG_ROM_LEVEL   (MICROPY_CONFIG_ROM_LEVEL_FULL_FEATURES)
#define MICROPY_ENABLE_COMPILER    (1)
#define MICROPY_ENABLE_GC          (1)
#define MICROPY_PY_GC              (1)
#define MICROPY_PY_SYS             (1)
#define MICROPY_PY_SYS_PLATFORM    "phi"
#define MICROPY_PY_BUILTINS_INPUT  (0)
#define MICROPY_PY_ASYNC_AWAIT     (1)

/* No real filesystem in this embedding — all scripts arrive as in-memory
 * text (exec'd via mp_embed_exec_str, same as assets/cube.gltf reached
 * wasm through --embed-file rather than a real fopen path), not loaded via
 * Python's `import` statement from disk. Everything below turns off code
 * paths that assume file-backed import/open/stdio-as-file-objects exist —
 * each one otherwise pulls in a symbol (mp_import_stat, mp_lexer_new_from_file,
 * mp_builtin_open_obj, mp_sys_std{in,out,err}_obj, mp_module_uctypes,
 * mp_hal_set_interrupt_char) that FULL_FEATURES enables by ROM-level default
 * but that only a real port with a filesystem/terminal actually defines.
 * Revisit if/when Phi scripts need real `import` of separate files. */
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)
#define MICROPY_PY_IO              (0)
#define MICROPY_PY_SYS_STDFILES    (0)
#define MICROPY_PY_UCTYPES         (0)
#define MICROPY_KBD_EXCEPTION      (0)

/* GC needs to scan CPU registers (in case the only reference to a live
 * object is sitting in one, not yet spilled to the stack) — the generic
 * helper's default path does this via inline asm per architecture, which
 * doesn't have a wasm32 case (there's no register-enumeration story that
 * makes sense for a stack machine). MICROPY_GCREGS_SETJMP switches to a
 * portable fallback (setjmp() spills all callee-saved registers to a jmp_buf
 * on the C stack, which the existing stack scan then covers) that works
 * everywhere, not just wasm — used on all three targets for one consistent
 * config rather than forking it per platform. */
#define MICROPY_GCREGS_SETJMP      (1)

/* Float support is OFF by default in MicroPython regardless of ROM level
 * (MICROPY_FLOAT_IMPL defaults to MICROPY_FLOAT_IMPL_NONE) — found the hard
 * way, via a "decimal numbers not supported" SyntaxError on a plain `1.0`
 * literal while prototyping @phi.node's socket-definition decorator
 * (phi.md, Phase 6). Single-precision to match the engine's own float
 * (not double) convention everywhere on the C side (renderer.c, gbuffer.c,
 * meshobject.c, ...) — no reason for Python-side numbers to be a different
 * width from the C values they end up feeding. */
#define MICROPY_FLOAT_IMPL         (MICROPY_FLOAT_IMPL_FLOAT)

/* Routes every print()/exception-traceback byte through mp_port.c's
 * phi_mp_capture_output() instead of the default MP_PLAT_PRINT_STRN
 * (mpconfig.h's fallback -> mphalport.c's mp_hal_stdout_tx_strn_cooked ->
 * plain printf), so the Console panel can actually display what a typed
 * command printed -- the real process stdout is invisible from inside
 * the game window. mp_plat_print (py/mpprint.c) is the ONLY output path
 * MicroPython uses here regardless (MICROPY_PY_IO/MICROPY_PY_SYS_STDFILES
 * are both off above, which compiles out the alternate sys.stdout-based
 * path), so this one override covers everything. */
void phi_mp_capture_output(const char *str, size_t len);
#define MP_PLAT_PRINT_STRN(str, len) phi_mp_capture_output(str, len)
