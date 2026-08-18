#pragma once
#include <stddef.h>
#include "meshobject.h"
#include "input.h"
#include "input_gamepad.h"
#include "phi_audio.h"

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

/* Registers the live state phi.prop_get/set and phi.enable_physics/
 * apply_impulse/get_velocity/set_velocity read/write through -- call
 * once from main(), mirrors console_set_target/asset_browser_set_
 * target's "single instance, registered once" pattern. `get_selected_
 * object` is main.c's own selected_mesh_object (Phase 5's real multi-
 * object scene graph, see scene_objects.h) -- called FRESH every time
 * (not snapshotted), since which object is selected changes constantly;
 * `target="object"` in Python resolves to whatever it returns right now
 * (NULL = nothing selected/a Light is selected instead). `edit_face` is
 * still read fresh via its own address (main.c's g_edit_face changes
 * every frame too) -- `target="face"` resolves to `get_selected_
 * object()->hem->faces[*edit_face]` once a face is actually selected.
 * `phys_world` (see phi.md's "Bullet Physics via Emscripten") is what
 * phi.enable_physics/apply_impulse/get_velocity/set_velocity operate
 * against -- the same shared world main.c's own CTX_ACTION_ENABLE_
 * PHYSICS handler and main_loop's step/sync use, not a separate one. */
void phi_mp_register_targets(MeshObject *(*get_selected_object)(void), const int *edit_face,
                              PhiPhysicsWorld *phys_world);

/* Phase 9 gap-closing (2026-08-18, see phi.md's Phase 9 "Known gaps") --
 * three independent registrations, each a plain pointer handoff like
 * phi_mp_register_targets above, but each usable on its own without the
 * selection concept that function bundles in: player_main.c (no
 * selection, no editor UI) calls these directly; editor_main.c doesn't
 * need phi_mp_register_input or phi_mp_register_camera_callback (it
 * drives its own camera and input handling itself) but DOES still reach
 * the object-id-keyed transform/physics bindings for free, since those
 * aren't tied to any of these three registrations. */

/* phi.set_camera(x, y, z, yaw, pitch) -- a function-pointer handoff, NOT
 * a raw Renderer* (deliberately: mp_port.c/mp_port.h must never need
 * renderer.h's real GL-touching renderer.c linked in, the same "thin API,
 * no heavy link dependency" discipline this header's own top comment and
 * mp_geometry_test/mp_node_test's Makefile targets already depend on --
 * see mp_port.c's own comment at the registration site). player_main.c
 * passes a small wrapper that calls the real renderer_set_camera. */
void phi_mp_register_camera_callback(void (*set_camera)(Vec3f eye, float yaw, float pitch));

/* phi.object_enable_physics/object_apply_impulse/object_get_velocity/
 * object_set_velocity -- the object-id-keyed physics functions. Reuses
 * the same underlying physics-world pointer phi_mp_register_targets sets
 * (see mp_port.c) rather than a second one to keep in sync; call this
 * INSTEAD of phi_mp_register_targets when there's no selection concept
 * to register (player_main.c), or leave it uncalled where phi_mp_
 * register_targets already covers it (editor_main.c). */
void phi_mp_register_physics_world(PhiPhysicsWorld *phys_world);

/* phi.key_down/mouse_pos/mouse_button_down -- reads live InputState
 * (see input.h). `inp` is borrowed, not copied -- must outlive every
 * subsequent phi_mp_exec call, same "read fresh every call, not
 * snapshotted" convention phi_mp_register_targets' get_selected_object
 * callback already uses for its own live state. Safe to include input.h
 * here: it's just a struct type + prototypes, no function this file
 * calls, so it adds no link dependency (unlike input_gamepad.h below,
 * which IS called directly -- see that one's own comment for why it
 * needs the function-pointer treatment instead). */
void phi_mp_register_input(const InputState *inp);

/* phi.gamepad_count/gamepad_connected/gamepad_button/gamepad_axis --
 * function-pointer handoff, NOT calling phi_gamepad_count/phi_gamepad_
 * get_state (input_gamepad.h) directly, for the identical reason phi_mp_
 * register_camera_callback above doesn't call renderer_set_camera
 * directly: those real functions are only ever DEFINED by a real backend
 * (input_gamepad_native.c, which pulls in the entire vendored SDL2 tree)
 * -- linking that into mp_geometry_test/mp_node_test's lightweight,
 * intentionally-GL-and-SDL2-free Makefile targets is exactly the
 * regression this indirection avoids. player_main.c passes phi_gamepad_
 * count/phi_gamepad_get_state themselves as the two callbacks -- same
 * functions, just handed over by pointer instead of called directly. */
void phi_mp_register_gamepad_callbacks(int (*count)(void), const PhiGamepadState *(*get_state)(int index));

/* phi.load_sound/play_sound/play_sound_3d/stop_sound (Phase 10, see
 * phi.md's "Phase 10 -- Audio"). Function-pointer handoff, NOT calling
 * phi_audio_load_sound/play/play_3d/stop directly, for the identical
 * reason phi_mp_register_camera_callback/_gamepad_callbacks above don't
 * call their own real functions directly -- audio_native.c may pull in
 * real ALSA; mp_geometry_test/mp_node_test/mp_phase9_gap_test must never
 * need that linked in. player_main.c hands over those exact four real
 * functions by pointer, same shape as the gamepad registration just
 * above. phi_audio_set_listener is NOT exposed to Python -- player_
 * main.c calls it directly, every frame, from the camera's own current
 * position/basis, so 3D positional audio automatically tracks whatever
 * the camera is doing (including a script's own phi.set_camera calls)
 * with no separate Python-side bookkeeping needed. */
void phi_mp_register_audio_callbacks(
    PhiSound *(*load_sound)(const char *path),
    PhiAudioVoice (*play)(PhiSound *sound, float volume, int loop),
    PhiAudioVoice (*play_3d)(PhiSound *sound, Vec3f position, float volume, int loop),
    void (*stop)(PhiAudioVoice voice)
);

/* Registers the real render-a-still-frame callback phi.render() calls
 * (see mp_port.c's native_render) -- main.c passes its own render_
 * still_frame_to_disk (Phase 3's real path tracer, see path_tracer.h),
 * same "give mp_port.c a function pointer to main.c's own logic rather
 * than duplicating it" shape phi_mp_register_targets already uses for
 * data pointers. cb must write a NUL-terminated path into out_path (cap
 * bytes) and return 1 on success, 0 on failure (empty scene, write
 * error) -- native_render raises ValueError on a 0 return rather than
 * returning a sentinel Python could silently ignore. */
void phi_mp_register_render_callback(int (*cb)(char *out_path, size_t cap));

/* Registers the real ragdoll-activation callback phi.activate_ragdoll()
 * calls (Phase 4, see ragdoll.h) -- main.c passes a small wrapper that
 * calls ragdoll_activate against its own g_skinned_test_obj/g_phys_world,
 * same function-pointer-handoff shape phi_mp_register_render_callback
 * just above already uses. cb returns the number of ragdoll bodies
 * actually spawned (0 = nothing to activate, e.g. the skinned test
 * object never loaded) -- native_activate_ragdoll returns that count to
 * Python directly rather than raising on 0, since "nothing loaded yet"
 * is a real, unsurprising outcome, not a usage error. */
void phi_mp_register_ragdoll_callback(int (*cb)(void));

/* Registers real playback control over main.c's g_skinned_test_obj (the
 * one existing SkinnedMeshObject slot, see phi.md's Phase 4 status --
 * this codebase has no multi-object registry for skinned meshes yet, the
 * same "single global slot" stage MeshObject itself was at before Phase
 * 5's real scene_objects.c registry landed) -- same function-pointer-
 * handoff shape phi_mp_register_render_callback/_ragdoll_callback above
 * already use. Real, honestly-scoped gap this closes: this engine had
 * real C-side Armature/AnimClip/Playback and GPU skinning with ZERO
 * Python bindings before this.
 *   play(clip_name, loop): clip_name NULL/empty picks clip 0 if any
 *     exist; returns 1 on success, 0 if nothing loaded or no such clip.
 *   set_playing/get_playing: pause/resume without restarting from time 0
 *     (unlike play, which always restarts).
 *   get_time/set_time: read/scrub the current playback time in seconds;
 *     set_time returns 0 if nothing is currently playing (no clip to
 *     scrub).
 *   list_clips(out_names, out_durations, max_clips): fills up to
 *     max_clips entries (each name up to 63 chars + NUL), returns how
 *     many were written -- so a script (or Claude) can discover real
 *     clip names instead of guessing them, the same reason phi.md's
 *     Phase 6 section gives for phi.node_types() existing. */
void phi_mp_register_animation_callbacks(
    int   (*play)(const char *clip_name, int loop),
    void  (*set_playing)(int playing),
    int   (*get_playing)(void),
    float (*get_time)(void),
    int   (*set_time)(float t),
    int   (*list_clips)(char out_names[][64], float *out_durations, int max_clips)
);

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
