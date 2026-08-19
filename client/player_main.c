/* Phase 9's "chromeless gameloop" -- see phi.md's "Editor/Player split".
 * This is player_main.c, NOT editor_main.c with things deleted: it's a
 * new, independent driver that links against the exact same engine core
 * modules (renderer.c/gbuffer.c/phi_physics.cpp/scene_objects.c/node_graph.c/
 * render_hooks.c/mp_port.c/input_gamepad_*.c -- see the Makefile's
 * ENGINE_CORE_SRCS) as the editor, minus every editor-only system: no
 * ui.c panel layout, no Asset Browser, no Chat panel, no Console REPL, no
 * net.c/server.py connection, no hardcoded startup test MeshObject, no
 * "selected object" concept (see phi_mp_register_targets' own comment --
 * that's a real UI notion the editor has and a shipped game doesn't).
 *
 * Boots straight into: init the engine core -> load `./game/` -> real
 * per-frame gameplay loop. `./game/` is resolved relative to the process's
 * own working directory (matching every other relative asset path this
 * codebase already uses, e.g. "assets/test/armature_test.gltf" in
 * editor_main.c) -- a shipped game is expected to run with game/ as a
 * sibling of the executable, or from a working directory that has it.
 *
 * Two mutually exclusive entry-point shapes (see phi.md's "./game/
 * directory" section), selected at COMPILE TIME by the Makefile
 * (PHI_GAME_HAS_C_ENTRY, defined iff game/src/main.c exists when `make
 * player` runs -- see Makefile's GAME_SRC_FILES/PHI_GAME_HAS_C_ENTRY):
 *
 *   - game/src/main.c present: real exported C symbols (game_init/
 *     game_tick/game_shutdown below), called directly against phi.h --
 *     "no MicroPython round trip in the hot path", per phi.md. This build
 *     doesn't initialize MicroPython at all.
 *   - game/main.py present (and no game/src/main.c): loaded once via
 *     phi_mp_exec, then a real tick(dt) Python call every frame.
 *
 * phi.md also documents a THIRD shape -- a game/main.py-driven game with
 * ADDITIONAL game/src/ *.c files exposed back into Python as custom
 * phi.*-style bound functions, for hand-written performance-critical code from
 * a mostly-Python game. That needs a real binding-registration mechanism
 * (something like mp_port.c's own MP_DEFINE_CONST_FUN_OBJ pattern, but
 * driven by user code rather than engine code) that does not exist yet --
 * a real, separate future addition, not silently promised by this file. */
#include "phi_platform.h"
#include "renderer.h"
#include "gbuffer.h"
#include "scene_objects.h"
#include "skinned_scene_objects.h"
#include "meshobject.h"
#include "halfedge_gltf.h"
#include "skinned_mesh.h"
#include "texture_cache.h"
#include "phi_physics.h"
#include "node_graph.h"
#include "render_hooks.h"
#include "input_gamepad.h"
#include "input.h"
#include "phi_audio.h"
#include "vec3.h"
#include <math.h>
#include <stdlib.h>   /* malloc/free -- read_whole_file below needs this unconditionally, not just in the #else (Python) branch; a real bug clang caught (implicit-declaration error) that GCC had silently let through as a warning */

#ifdef PHI_GAME_HAS_C_ENTRY
/* Provided by the user's own game/src/main.c -- statically linked into
 * this exact binary (see this file's own top comment and the Makefile's
 * PLAYER_SRCS). Not declared in phi.h: these are symbols the GAME
 * exports to the engine, the reverse direction of everything else phi.h
 * declares. game_init receives the three live engine instances a C game
 * actually needs immediate access to (Phase 9 gap-closing, 2026-08-18 --
 * see phi.md's Phase 9 "Known gaps" and phi.h's own input.h/renderer.h
 * comments) -- the same real renderer/physics-world/input state the
 * Python path reaches via phi.set_camera, phi.object_enable_physics, and
 * phi.key_down, handed over directly instead, since there's no MicroPython round trip
 * on this path at all. game_tick/game_shutdown take no parameters --
 * game code is expected to stash whatever pointers it needs during
 * game_init, the same "capture once, use every frame" shape player_
 * main.c's own g_renderer/g_phys_world/g_inp already follow. */
extern void  game_init(Renderer *renderer, PhiPhysicsWorld *phys_world, const InputState *input);
extern void  game_tick(float dt);
extern void  game_shutdown(void);
#else
#include "mp_port.h"
#endif

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <stdio.h>
#include <string.h>

static Renderer        *g_renderer   = NULL;
static GBuffer          *g_gbuf       = NULL;
static PhiPhysicsWorld  *g_phys_world = NULL;
static double            g_last_t     = 0.0;
static InputState        g_inp;

#ifndef PHI_GAME_HAS_C_ENTRY
/* phi.set_camera's real callback (Phase 9 gap-closing, 2026-08-18 -- see
 * phi.md's Phase 9 "Known gaps" and mp_port.h's phi_mp_register_camera_
 * callback comment for why this is a function pointer, not a raw
 * Renderer* handed to mp_port.c directly). */
static void player_set_camera(Vec3f eye, float yaw, float pitch) {
    renderer_set_camera(g_renderer, eye, yaw, pitch);
}
#endif

#ifndef PHI_GAME_HAS_C_ENTRY
/* Set once at startup: whether game/main.py defined a real, callable
 * tick(dt) -- checked once here rather than re-checking (and potentially
 * re-printing an AttributeError traceback) every single frame. A game
 * script with no tick() at all just renders a static scene forever,
 * which is a real, valid (if unusual) thing to author -- not an error. */
static int g_have_py_tick = 0;
#endif

/* Reads an entire file into a malloc'd, NUL-terminated buffer. Returns
 * NULL if it doesn't exist or can't be read -- a missing game/main.py is
 * a real, expected outcome for a game/src/main.c-shaped build (or an
 * as-yet-empty ./game/ during early development), not a fatal error. */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    return buf;
}

/* Keeps phi_audio.h's 3D listener in sync with the real, current camera
 * every frame (Phase 10, see phi.md's "Phase 10 -- Audio") -- so
 * positional audio (phi.play_sound_3d) automatically tracks wherever the
 * camera actually is, including a script's own phi.set_camera calls,
 * with no separate Python-side bookkeeping needed (see mp_port.h's
 * phi_mp_register_audio_callbacks comment). fwd/right derived from cam_
 * yaw/cam_pitch via the identical basis formula editor_main.c's own
 * cam_basis uses (renderer.c's build_vp/mat4_look_dir), so panning is
 * consistent with whatever's actually rendered, not a separately-
 * invented convention. */
static void update_audio_listener(void) {
    float yaw = g_renderer->cam_yaw, pitch = g_renderer->cam_pitch;
    float sy = sinf(yaw), cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    Vec3f fwd   = { -sy*cp, sp, -cy*cp };
    Vec3f right = { cy, 0.0f, -sy };
    Vec3f pos = { g_renderer->cam_pos[0], g_renderer->cam_pos[1], g_renderer->cam_pos[2] };
    phi_audio_set_listener(pos, fwd, right);
}

static void player_render(void) {
    int w, h;
    phi_platform_get_window_size(&w, &h);
    if (w < 4 || h < 4) return;

    /* No panel sub-rectangle here (see this file's own top comment --
     * there's no ui.c in this build at all) -- the Scene panel's whole
     * job in the editor was carving out ITS rectangle from the rest of
     * the window; a chromeless player IS that rectangle, the full
     * window, every frame. Re-issued every frame (not just on a resize
     * event) to match editor_main.c's own draw_panel_scene convention,
     * which does the same for exactly the same reason: no separate
     * resize-event plumbing needed, just always-correct current size. */
    renderer_resize(g_renderer, w, h);
    gbuffer_resize(g_gbuf, w, h);
    gbuffer_set_viewport_offset(g_gbuf, 0, 0);

    float sky[3];
    renderer_get_sky_color(sky);
    static const float light_dir[3] = {0.577f, 0.577f, 0.577f};

    gbuffer_begin_geometry_pass(g_gbuf, sky);
    {
        MeshObject *objects[SCENE_MAX_OBJECTS];
        int n_objects = scene_object_get_all(objects);
        for (int i = 0; i < n_objects; i++) renderer_draw_mesh_object(g_renderer, objects[i]);
        renderer_draw_lights(g_renderer);
        /* Every live skinned scene object -- object-id range 7000+id, same
         * convention editor_main.c's own scene_content_cb uses. */
        SkinnedMeshObject *skinned_objs[SKINNED_SCENE_MAX_OBJECTS];
        int n_skinned = skinned_scene_object_get_all(skinned_objs);
        for (int i = 0; i < n_skinned; i++) {
            renderer_draw_skinned_mesh(g_renderer, skinned_objs[i],
                                        7000u + (unsigned int)skinned_scene_object_id(skinned_objs[i]));
        }
    }
    /* See editor_main.c's own scene_content_cb/draw_panel_scene comment
     * on why NULL here: no shadow-casting geometry source in the generic
     * VERTEX_STRIDE=7 format gbuffer_render_shadow_map's shader assumes
     * exists in this codebase right now -- MeshObject uses a different,
     * incompatible stride and doesn't opt into shadow casting. Passing
     * NULL is honest about that; the function no-ops safely on it. */
    gbuffer_render_shadow_map(g_gbuf, NULL, light_dir);
    float inv_vp[16];
    renderer_get_inverse_view_proj(g_renderer, inv_vp);
    gbuffer_resolve(g_gbuf, light_dir, sky, inv_vp, g_renderer->cam_pos);
    renderer_end_frame(g_renderer);

    phi_platform_swap();
}

static void player_loop(void *userdata) {
    (void)userdata;
    double now = phi_platform_now();
    float dt = (float)(now - g_last_t);
    g_last_t = now;
    if (dt > 0.05f) dt = 0.05f;   /* cap at 50ms -- same convention as editor_main.c's main_loop */

    phi_gamepad_poll();
    update_audio_listener();

    phi_physics_world_step(g_phys_world, dt);
    /* Every live skinned scene object (skinned_scene_objects.h) -- real
     * per-frame CPU pose->world->skin advance, same convention editor_
     * main.c's own main_loop uses for its skinned test object/registry. */
    {
        SkinnedMeshObject *skinned_objs[SKINNED_SCENE_MAX_OBJECTS];
        int n_skinned = skinned_scene_object_get_all(skinned_objs);
        for (int i = 0; i < n_skinned; i++) skinned_mesh_object_update(skinned_objs[i], dt);
    }
    {
        MeshObject *objects[SCENE_MAX_OBJECTS];
        int n_objects = scene_object_get_all(objects);
        for (int i = 0; i < n_objects; i++) {
            if (!objects[i]->phys_body) continue;
            float orientation[4];
            phi_physics_get_transform(objects[i]->phys_body, &objects[i]->position, orientation);
            objects[i]->orientation.x = orientation[0];
            objects[i]->orientation.y = orientation[1];
            objects[i]->orientation.z = orientation[2];
            objects[i]->orientation.w = orientation[3];
        }
    }

    /* Mouse capture policy ("steal the mouse until Escape" -- see input.h's
     * own input_capture_mouse comment for the real per-platform mechanism):
     * click to engage, Escape to release. A real, deliberate policy choice
     * that lives here (player_main.c), not in input.c -- input.c only
     * knows HOW to grab/hide/warp the pointer, not WHEN a game should want
     * that to happen. The very click that engages capture is consumed
     * here (not left for game code to also see) so it doesn't
     * additionally register as e.g. a "shoot" action the same frame. */
    if (!input_mouse_captured() && g_inp.lmb_click) {
        input_capture_mouse(1);
        g_inp.lmb_click = 0;
    } else if (input_mouse_captured() && g_inp.escape_edge) {
        input_capture_mouse(0);
    }

#ifdef PHI_GAME_HAS_C_ENTRY
    game_tick(dt);
#else
    if (g_have_py_tick) {
        char call[64];
        snprintf(call, sizeof(call), "tick(%.6f)", (double)dt);
        char *out = phi_mp_exec(call);
        if (out[0]) fputs(out, stdout);   /* real print()/traceback output from the game's own tick -- surfaced, not swallowed */
        free(out);
    }
#endif

    /* Drains every one-shot InputState field (see input.h's own "producer
     * sets, consumer clears" convention) once per frame, AFTER game code
     * has had a chance to read them -- in the editor, ui.c's click
     * routing is that consumer; there is no ui.c here, so nothing was
     * ever clearing these in the player build until now, meaning e.g.
     * lmb_click would latch to 1 forever after the very first click
     * (real bug: a game/src/main.c reading it for "click to shoot" would
     * fire every single frame after the first click, not once). Held
     * state (lmb_down/keys_down/mouse_x/y) is untouched -- only the
     * real one-shot edges/queues. */
    g_inp.lmb_click = g_inp.rmb_click = g_inp.mmb_click = 0;
    g_inp.enter_edge = g_inp.backspace_edge = 0;
    g_inp.histup_edge = g_inp.histdown_edge = 0;
    g_inp.tab_edge = g_inp.escape_edge = 0;
    g_inp.scroll_delta = 0;
    g_inp.typed_count = 0;
    g_inp.mouse_dx = g_inp.mouse_dy = 0;

    player_render();
}

int main(void) {
    int mp_stack_top_marker;
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
    printf("[player] Initialising Phi (chromeless player build)...\n");

    PhiPlatformConfig pcfg = { .title = "Phi Game", .width = 1280, .height = 720 };
    phi_platform_init(&pcfg);
    int w, h;
    phi_platform_get_window_size(&w, &h);
    g_renderer = renderer_create(w, h);
    g_gbuf = gbuffer_create(w, h);

    /* Real texture loading for glTF imports -- see editor_main.c's own
     * identical registration for why this must run after renderer_create
     * (GL context) and before any glTF load. */
    halfedge_gltf_register_texture_loader(texture_cache_load);
    skinned_mesh_register_texture_loader(texture_cache_load);

    /* Real, non-degenerate starting vantage point (Phase 9 gap-closing,
     * 2026-08-18 -- see phi.md's Phase 9 "Known gaps": this used to be
     * left at renderer_create's calloc-zeroed default, (0,0,0) looking
     * nowhere in particular, which is why nothing was ever watchable in
     * this build before). Not meant to suit every game -- a real game
     * overrides it via phi.set_camera (Python) or renderer_set_camera
     * directly (C, via phi.h) -- just a sane fallback so an otherwise-
     * untouched scene is actually visible rather than a black/undefined
     * frame. Values chosen to echo editor_main.c's own initial vantage
     * (g_cam_pivot/g_cam_distance's starting values there), not derived
     * from them -- the player has no cam_recompute_pos orbit state to
     * share. */
    renderer_set_camera(g_renderer, (Vec3f){128.0f, 120.0f, 40.0f}, 0.0f, -0.4f);

    scene_objects_init();
    skinned_scene_objects_init();
    phi_graph_system_init();
    render_hooks_init();
    phi_gamepad_init();

    /* Keyboard/mouse input (Phase 9 gap-closing, 2026-08-18 -- see
     * phi.md's Phase 9 "Known gaps": this build had no input source at
     * all before this). Real platform event callbacks, same input.c
     * mechanism editor_main.c already uses -- phi_platform's own main-
     * loop event pump (see phi_platform_native.c) drives it automatically
     * every frame from here on, no per-frame poll call needed. */
    input_init(&g_inp);
    input_install_callbacks(&g_inp);

    g_phys_world = phi_physics_world_create();

    /* Audio (Phase 10, see phi.md's "Phase 10 -- Audio") -- real ALSA
     * playback if this build has it (see the Makefile's ALSA_HEADER
     * detection), a real honest "no device" fallback otherwise; sounds
     * still load/decode for real either way. */
    phi_audio_init();

#ifdef PHI_GAME_HAS_C_ENTRY
    printf("[player] game/src/main.c entry point -- calling game_init()\n");
    game_init(g_renderer, g_phys_world, &g_inp);
#else
    /* mp_stack_top_marker's ADDRESS (not value) is the conservative GC
     * stack-scan boundary -- see mp_port.h's phi_mp_init() comment for
     * why it must be a local at main()'s own top-level scope. Declared
     * above regardless of which branch runs so its address is always
     * valid at this exact stack depth, matching editor_main.c's own
     * mp_stack_top convention. */
    phi_mp_init(&mp_stack_top_marker);

    /* Phase 9 gap-closing (2026-08-18, see phi.md's Phase 9 "Known
     * gaps"): registers phi.set_camera, phi.object_enable_physics/
     * apply_impulse/get_velocity/set_velocity, phi.key_down/mouse_pos/
     * mouse_button_down, and phi.gamepad_count/connected/button/axis --
     * see mp_port.h's own comments on each for why these are the right
     * shape (function-pointer handoffs for camera/gamepad, a borrowed
     * live pointer for input, reusing s_phys_world for physics) rather
     * than phi_mp_register_targets, which needs a "selected object"
     * concept this chromeless build doesn't have at all. */
    phi_mp_register_camera_callback(player_set_camera);
    phi_mp_register_physics_world(g_phys_world);
    phi_mp_register_input(&g_inp);
    phi_mp_register_gamepad_callbacks(phi_gamepad_count, phi_gamepad_get_state);
    phi_mp_register_audio_callbacks(phi_audio_load_sound, phi_audio_play, phi_audio_play_3d, phi_audio_stop);

    char *script = read_whole_file("game/main.py");
    if (script) {
        printf("[player] game/main.py found -- executing\n");
        char *out = phi_mp_exec(script);
        if (out[0]) fputs(out, stdout);
        free(out);
        free(script);

        /* Real, callable tick(dt)? Checked once via a bare expression
         * (phi_mp_exec's REPL-style auto-print -- see mp_port.c's own
         * note on this being intentional -- prints "True"/"False" here,
         * which this parses directly rather than needing a second
         * round trip). */
        char *check = phi_mp_exec("callable(globals().get('tick'))");
        g_have_py_tick = (strncmp(check, "True", 4) == 0);
        free(check);
        printf("[player] game/main.py defines tick(dt): %s\n", g_have_py_tick ? "yes" : "no");
    } else {
        printf("[player] WARNING: no game/main.py and no game/src/main.c -- "
               "nothing to run, showing an empty scene\n");
    }
#endif

    g_last_t = phi_platform_now();
    phi_platform_set_main_loop(player_loop, NULL);

#ifdef PHI_GAME_HAS_C_ENTRY
    game_shutdown();
#endif
    phi_audio_shutdown();
    phi_platform_shutdown();
    return 0;
}
