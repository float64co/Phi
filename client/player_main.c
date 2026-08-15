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
 * ADDITIONAL game/src/*.c files exposed back into Python as custom phi.*-
 * style bound functions, for hand-written performance-critical code from
 * a mostly-Python game. That needs a real binding-registration mechanism
 * (something like mp_port.c's own MP_DEFINE_CONST_FUN_OBJ pattern, but
 * driven by user code rather than engine code) that does not exist yet --
 * a real, separate future addition, not silently promised by this file. */
#include "phi_platform.h"
#include "renderer.h"
#include "gbuffer.h"
#include "scene_objects.h"
#include "meshobject.h"
#include "phi_physics.h"
#include "node_graph.h"
#include "render_hooks.h"
#include "input_gamepad.h"
#include "vec3.h"

#ifdef PHI_GAME_HAS_C_ENTRY
/* Provided by the user's own game/src/main.c -- statically linked into
 * this exact binary (see this file's own top comment and the Makefile's
 * PLAYER_SRCS). Not declared in phi.h: these are symbols the GAME
 * exports to the engine, the reverse direction of everything else phi.h
 * declares. */
extern void  game_init(void);
extern void  game_tick(float dt);
extern void  game_shutdown(void);
#else
#include "mp_port.h"
#include <stdlib.h>
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

    phi_physics_world_step(g_phys_world, dt);
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

    scene_objects_init();
    phi_graph_system_init();
    render_hooks_init();
    phi_gamepad_init();

    g_phys_world = phi_physics_world_create();

#ifdef PHI_GAME_HAS_C_ENTRY
    printf("[player] game/src/main.c entry point -- calling game_init()\n");
    game_init();
#else
    /* mp_stack_top_marker's ADDRESS (not value) is the conservative GC
     * stack-scan boundary -- see mp_port.h's phi_mp_init() comment for
     * why it must be a local at main()'s own top-level scope. Declared
     * above regardless of which branch runs so its address is always
     * valid at this exact stack depth, matching editor_main.c's own
     * mp_stack_top convention. */
    phi_mp_init(&mp_stack_top_marker);

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
    phi_platform_shutdown();
    return 0;
}
