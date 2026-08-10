#pragma once
#include <stddef.h>
#include "meshobject.h"

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

/* ---- DNA/RNA property system + @phi.panel, exposed to Python ----
 * (see phi.md's "Property System (DNA/RNA analogue)" / "Python-
 * extensible panels" and phi_prop.h). phi_mp_init() installs a bootstrap
 * script defining a `phi` namespace with `phi.prop_get`/`phi.prop_set`
 * (thin wrappers over phi_prop_registry.c's registries) and
 * `phi.panel`/`phi.Panel` (the exact @phi.panel class-decorator pattern
 * already prototyped and confirmed working against real MicroPython in
 * mp_test_main.c step 4 -- this reuses that proven pattern rather than
 * inventing a new one). */

/* Registers the live pointers phi.prop_get/set read/write through --
 * call once from main(), mirrors console_set_target/
 * asset_browser_set_target's "single instance, registered once"
 * pattern. `test_obj_loaded`/`edit_face` are read fresh on every call
 * (not snapshotted at registration time), since main.c's g_test_mesh_
 * loaded/g_edit_face change every frame -- pass their addresses, not
 * their values. `target="object"` in Python resolves to `test_obj`
 * (once loaded); `target="face"` resolves to
 * `test_obj->hem->faces[*edit_face]` (once a face is actually selected).
 * `phys_world` (see phi.md's "Bullet Physics via Emscripten") is what
 * phi.enable_physics/apply_impulse/get_velocity/set_velocity operate
 * against -- the same shared world main.c's own CTX_ACTION_ENABLE_
 * PHYSICS handler and main_loop's step/sync use, not a separate one. */
void phi_mp_register_targets(MeshObject *test_obj, const int *test_obj_loaded, const int *edit_face,
                              PhiPhysicsWorld *phys_world);

/* Number of currently-registered @phi.panel classes. Panels are captured
 * (name + a freshly instantiated, cached instance) the moment their
 * decorator runs, via a native callback the bootstrap's @phi.panel
 * decorator invokes -- see mp_port.c -- rather than C ever needing to
 * introspect the Python-side registry dict directly. */
int phi_mp_panel_count(void);

/* Name (the string literal passed to @phi.panel(...)) of the panel at
 * `index`, 0 <= index < phi_mp_panel_count(). Returned pointer is into
 * this module's own static storage (a plain C copy taken at registration
 * time, not a MicroPython object) -- valid for the process's lifetime,
 * no need to copy it. */
const char *phi_mp_panel_name(int index);

/* Calls panel `index`'s cached instance's draw(ctx) (ctx == the same
 * `phi` namespace object every panel sees, exposing prop_get/prop_set --
 * see phi.md's note on this being a deliberate simplification of the
 * `ctx.prop(obj, name)` sketch, not the literal signature), and expects
 * it to return a plain list of strings, each one row of text to render
 * -- no interactive widgets (buttons, separators) this pass, see phi.md.
 * Writes up to `max_lines` of them into `out_lines` (each truncated to
 * `line_cap`-1 bytes), returns how many were written. Returns -1 if the
 * call raised (the traceback is captured into the same buffer
 * phi_mp_exec's output is, readable afterward via
 * phi_mp_last_captured_output() below -- unlike phi_mp_exec, this
 * doesn't return the text itself, since the common case is "print it to
 * the Console log only on failure", not every call) or if draw() didn't
 * return a list at all. */
int phi_mp_draw_panel(int index, char out_lines[][256], int max_lines);

/* Returns whatever the most recent phi_mp_draw_panel/phi_mp_exec call
 * printed or raised, WITHOUT resetting it (unlike phi_mp_exec, which
 * always starts a call by clearing this same buffer) -- borrowed
 * pointer, valid until the next phi_mp_* call, copy it if it needs to
 * outlive that. Never NULL; empty string if nothing's been captured yet. */
const char *phi_mp_last_captured_output(void);
