#include "octree.h"
#include "octree_render.h"
#include "octree_stl.h"
#include "physics.h"
#include "renderer.h"
#include "net.h"
#include "input.h"
#include "editor.h"
#include "console.h"
#include "phi_platform.h"
#include "halfedge_gltf.h"
#include "meshobject.h"
#include "font.h"
#include "svg_icon.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES3/gl3.h>   /* not GLES2/gl2.h — wasm renders through the same G-buffer path as native now */
#else
#include <GL/gl.h>
#endif
#include "gbuffer.h"

/* ---- Global state ---- */
static Octree      *g_world    = NULL;
static RenderMesh  *g_mesh     = NULL;
static Renderer    *g_renderer = NULL;
static GBuffer      *g_gbuf     = NULL;  /* deferred renderer, see gbuffer.h — both build targets now */
static GameState    g_gs       = {0};

/* Phase 1 foundation test object: loads assets/cube.gltf (a hand-authored
 * unit cube, see the asset's own generator script) through the glTF ->
 * half-edge -> RenderMesh pipeline (halfedge_gltf.c, meshobject.c) at
 * startup and renders it as a fixed, static, non-interactive object —
 * proving the data actually flows into the live scene. No editor
 * interaction, UI, or physics yet; see phi.md's Phase 1 section. */
static MeshObject   g_test_mesh_object = {0};
static int          g_test_mesh_loaded = 0;
static NetState     g_ns       = {0};
static InputState   g_inp      = {0};
static EditorState  g_ed       = {0};
static ConsoleState g_cs       = {0};
static double       g_last_t   = 0.0;
static float        g_net_timer = 0.0f;
static uint16_t     g_input_seq = 0;

/* ---- HUD overlay (via JS) ---- */
#ifdef __EMSCRIPTEN__
static void update_hud(int hp, int bots_alive, int local_id) {
    EM_ASM({
        var hud = document.getElementById('hud');
        if (hud) {
            hud.innerHTML =
                'HP: <b>' + $0 + '</b>' +
                ' &nbsp;|&nbsp; AMMO: <b>∞</b>' +
                ' &nbsp;|&nbsp; BOTS: <b>' + $1 + '</b>' +
                ' &nbsp;|&nbsp; ID: ' + $2 +
                ' &nbsp;|&nbsp; <span style="opacity:0.5">[F4] STL &nbsp; [E] Edit &nbsp; [`] Console</span>';
        }
    }, hp, bots_alive, local_id);
}
#else
static void update_hud(int hp, int bots_alive, int local_id) {
    (void)hp; (void)bots_alive; (void)local_id;
}
#endif

#ifdef __EMSCRIPTEN__
static void update_editor_ui(const EditorState *ed) {
    EM_ASM({
        var el = document.getElementById('editor-status');
        if (!el) return;
        if ($0) { el.style.display = 'block'; el.textContent = UTF8ToString($1); }
        else    { el.style.display = 'none'; }
    }, ed->active, ed->status);
}
#else
static void update_editor_ui(const EditorState *ed) { (void)ed; }
#endif

#ifdef __EMSCRIPTEN__
static void update_console_ui(const ConsoleState *cs) {
    if (!cs->open) {
        EM_ASM({
            var el = document.getElementById('console');
            if (el) el.style.display = 'none';
        });
        return;
    }
    char logbuf[CONSOLE_LOG_LINES * (CONSOLE_LINE_LEN + 1) + 1];
    logbuf[0] = 0;
    for (int i = 0; i < cs->log_count; i++) {
        strcat(logbuf, cs->log[i]);
        strcat(logbuf, "\n");
    }
    EM_ASM({
        var el      = document.getElementById('console');
        var logEl   = document.getElementById('console-log');
        var inputEl = document.getElementById('console-input');
        if (el) el.style.display = 'block';
        if (logEl)   logEl.textContent   = UTF8ToString($0);
        if (inputEl) inputEl.textContent = '] ' + UTF8ToString($1) + '_';
    }, logbuf, cs->input);
}
#else
static void update_console_ui(const ConsoleState *cs) { (void)cs; }
#endif

/* Loads assets/cube.gltf into g_test_mesh_object the same way main()'s
 * startup code originally did inline — factored out so the scene
 * right-click context menu's "Add > Mesh Object" (see
 * CTX_ACTION_ADD_MESH below) can reuse it instead of duplicating the
 * glTF -> half-edge -> RenderMesh pipeline call sequence. There's still
 * only ever one test-object slot (see g_test_mesh_object's own comment
 * below) — this spawns/reloads that one slot, it doesn't add a new
 * independent object to a list, since no such list exists yet. */
static void spawn_test_mesh_object(void) {
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    if (!hem) {
        printf("[main] failed to load assets/cube.gltf — MeshObject spawn skipped\n");
        return;
    }
    g_test_mesh_object.id = 1;
    /* Elevated well above normal bot/player ground-level traffic (~y=16
     * floor) — early native testing found bots occasionally standing
     * directly in the diagnostic camera's line of sight at ground-level
     * test positions, an environmental occlusion artifact from the live
     * multiplayer arena, not a rendering bug. */
    g_test_mesh_object.position = (Vec3f){128.0f, 100.0f, 90.0f};
    g_test_mesh_object.orientation = quat_identity();
    g_test_mesh_object.is_static = 1;
    g_test_mesh_object.render_mesh = mesh_create();
    /* assets/cube.gltf is a unit cube (half-extent 0.5) — scaled up 16x by
     * baking the scale directly into the flattened vertex positions (no
     * scale field on MeshObject yet, see meshobject.h). */
    for (int i = 0; i < hem->vert_count; i++)
        for (int a = 0; a < 3; a++)
            hem->verts[i].pos[a] *= 16.0f;
    meshobject_build_render_mesh_from_halfedge(g_test_mesh_object.render_mesh, hem, 4.0f);
    halfedge_destroy(hem);
    g_test_mesh_loaded = 1;
    printf("[main] loaded assets/cube.gltf as MeshObject: %d triangles\n",
           g_test_mesh_object.render_mesh->count / 3);
}

/* Frees the one test-object slot's GPU-side mesh and marks it unloaded —
 * scene_content_cb below already checks g_test_mesh_loaded before drawing
 * it, so clearing that flag is what actually makes it disappear. Clears
 * the UI's own selection too if it was pointing at this object, so
 * Properties doesn't keep showing a readout for something that no longer
 * exists. */
static void delete_test_mesh_object(void) {
    if (!g_test_mesh_loaded) return;
    if (ui_get_selected_object() == 4000u + (unsigned int)g_test_mesh_object.id) {
        ui_set_selected_object(0xFFFFFFFFu);
    }
    mesh_destroy(g_test_mesh_object.render_mesh);
    g_test_mesh_object.render_mesh = NULL;
    g_test_mesh_loaded = 0;
    printf("[main] deleted MeshObject\n");
}

/* Every renderer_draw_* call for the frame's game content — extracted
 * into a callback (see UIRenderContext.draw_scene_content in ui.h) so the
 * Scene panel can drive it between gbuffer_begin_geometry_pass() and
 * gbuffer_render_shadow_map() without ui.c needing to know about
 * Qek-specific entities (players/rockets) at all. */
static void scene_content_cb(void *userdata) {
    (void)userdata;
    renderer_draw_world(g_renderer, g_mesh);
    renderer_draw_ground_plane(g_renderer);
    renderer_draw_players(g_renderer, &g_gs, g_ns.local_id);
    renderer_draw_rockets(g_renderer, &g_gs);
    if (g_test_mesh_loaded) renderer_draw_mesh_object(g_renderer, &g_test_mesh_object);
    editor_render(&g_ed, g_renderer);
}

/* ---- Main loop ---- */
static void main_loop(void *userdata) {
    (void)userdata;
    double now = phi_platform_now();
    float dt = (float)(now - g_last_t);
    g_last_t = now;
    if (dt > 0.05f) dt = 0.05f;   /* cap at 50ms */

#ifndef __EMSCRIPTEN__
    net_poll_native();  /* wasm gets messages via an async JS callback instead */
#endif

    /* --- Apply any authoritative map swap from the server before physics/render --- */
    if (g_ns.has_pending_map) {
        g_ns.has_pending_map = 0;
        octree_destroy(g_world);
        g_world       = g_ns.pending_map;
        g_ns.pending_map = NULL;
        g_gs.world    = g_world;
        mesh_rebuild(g_mesh, g_world);
        mesh_upload(g_mesh);
        printf("[main] Swapped in server map (%d verts)\n", g_mesh->count);
    }

    /* --- UI layout + click routing --- must happen before the gameplay
     * input processing below: a UI click that's consumed (the panel
     * type-switcher icon toggling its dropdown, a dropdown row swapping a
     * panel, an Outliner row selection) must NOT also register as a
     * gameplay "fire" edge on that same physical LMB press. ui_layout()
     * needs to run before hit-testing (it's what computes every Area's
     * on-screen rect), and UIRenderContext is built once here and reused
     * for the ui_render() call further down too — one source of truth for
     * what got clicked vs. what actually gets drawn this frame, rather
     * than two separately-constructed contexts that could drift apart. */
    int cw, ch;
    phi_platform_get_window_size(&cw, &ch);
    ui_layout(cw, ch);

    static const float light_dir[3] = {0.577f, 0.577f, 0.577f};
    float sky[3];
    renderer_get_sky_color(sky);
    UIRenderContext ui_ctx = {0};
    ui_ctx.renderer = g_renderer;
    ui_ctx.gbuf = g_gbuf;
    ui_ctx.world_mesh = g_mesh;
    ui_ctx.gs = &g_gs;
    ui_ctx.local_player_id = g_ns.local_id;
    ui_ctx.test_obj = &g_test_mesh_object;
    ui_ctx.test_obj_loaded = g_test_mesh_loaded;
    ui_ctx.console = &g_cs;
    memcpy(ui_ctx.light_dir, light_dir, sizeof(light_dir));
    memcpy(ui_ctx.sky_color, sky, sizeof(sky));
    ui_ctx.draw_scene_content = scene_content_cb;

    /* Real clicks (lmb_click/rmb_click) — same "rising edge, drained and
     * cleared here" convention as fire/export_stl below. Routes into the
     * panel type-switcher icon/dropdown and Outliner row selection. A
     * consumed click must not ALSO fire a rocket on the same LMB press. */
    int ui_consumed_click = 0;
    if (g_inp.lmb_click) {
        g_inp.lmb_click = 0;
        ui_consumed_click = ui_on_mouse_button(g_inp.mouse_x, g_inp.mouse_y, 0, 1, &ui_ctx);
    }
    if (g_inp.rmb_click) {
        g_inp.rmb_click = 0;
        /* First offer the click to existing UI chrome (dismisses an
         * already-open context/type-switcher menu, same as any other
         * right-click ui_on_mouse_button already handles). If nothing
         * claimed it and the click landed inside the Scene panel's own
         * content rect (not its chrome) — and the octree editor isn't
         * active, which already owns RMB for carve-drags, same reasoning
         * the fire-gating above uses for LMB — open the scene context
         * menu there. This is what actually feeds
         * ui_open_scene_context_menu(x, y) the input phi.md's Native UI
         * System section previously flagged as its missing source. */
        int rmb_consumed = ui_on_mouse_button(g_inp.mouse_x, g_inp.mouse_y, 1, 1, &ui_ctx);
        if (!rmb_consumed && !g_ed.active) {
            float sx, sy, sw, sh;
            if (ui_get_scene_rect(&sx, &sy, &sw, &sh) &&
                (float)g_inp.mouse_x >= sx && (float)g_inp.mouse_x < sx + sw &&
                (float)g_inp.mouse_y >= sy && (float)g_inp.mouse_y < sy + sh) {
                ui_open_scene_context_menu(g_inp.mouse_x, g_inp.mouse_y);
            }
        }
    }
    if (ui_consumed_click) g_inp.fire = 0;

    /* Scene context-menu action, drained once per frame like the click
     * flags above. Only Add Mesh Object / Delete are wired to real
     * behavior — Frame Selected/Frame All/Deselect All are still
     * placeholder rows (see ui_poll_context_menu_action's own comment). */
    switch (ui_poll_context_menu_action()) {
        case CTX_ACTION_ADD_MESH:
            if (g_test_mesh_loaded) {
                printf("[main] context menu Add > Mesh Object: already exists "
                       "(only one test-object slot for now)\n");
            } else {
                spawn_test_mesh_object();
            }
            break;
        case CTX_ACTION_DELETE:
            if (g_test_mesh_loaded && ui_get_selected_object() == 4000u + (unsigned int)g_test_mesh_object.id) {
                delete_test_mesh_object();
            } else {
                printf("[main] context menu Delete: no MeshObject selected\n");
            }
            break;
        default:
            break;
    }

    /* --- Input processing --- */
    Player *local = NULL;
    for (int i = 0; i < g_gs.num_players; i++) {
        if (g_gs.players[i].id == (uint8_t)g_ns.local_id) {
            local = &g_gs.players[i]; break;
        }
    }

    if (local && local->alive) {
        console_update(&g_cs, &g_inp, &g_ed, &g_ns, &g_gs, g_renderer, local);
        editor_update(&g_ed, &g_gs, local, &g_inp, &g_ns, dt);

        if (g_ed.world_dirty) {
            mesh_rebuild(g_mesh, g_world);
            mesh_upload(g_mesh);
        }

        if (!g_ed.active) {
            /* Check ground BEFORE applying input so ground accel works on frame 1 */
            local->on_ground = physics_check_ground(g_gs.world, local->pos);
            local->crouching = g_inp.crouch;

            /* Apply input to local player prediction */
            physics_apply_input(local,
                                 g_inp.forward, g_inp.back, g_inp.left, g_inp.right,
                                 g_inp.jump,
                                 g_inp.yaw, g_inp.pitch,
                                 dt, local->on_ground);

            /* Fire rocket on rising edge — locally if no server, via net if connected */
            if (g_inp.fire) {
                if (g_ns.connected) {
                    net_send_fire(&g_ns, g_inp.yaw, g_inp.pitch, local->crouching);
                } else {
                    physics_fire_rocket(&g_gs, local);
                }
                g_inp.fire = 0;
            }
        } else {
            /* Editor mode owns movement (fly_move) and LMB/RMB (edit drags);
             * discard any fire edge so editing clicks never also shoot. */
            g_inp.fire = 0;
        }
    }

    /* STL export */
    if (g_inp.export_stl) {
        g_inp.export_stl = 0;
        printf("[main] Exporting STL...\n");
        octree_stl_download(g_world);
    }

    /* --- Physics tick (client-side prediction) --- */
    physics_update(&g_gs, dt);

    /* --- Network: send input at 20Hz --- */
    g_net_timer += dt;
    if (g_net_timer >= 0.05f && g_ns.connected) {
        g_net_timer = 0.0f;
        uint8_t flags = input_get_key_flags(&g_inp);
        if (g_ed.active)       flags |= KEY_EDITING;
        if (local && local->god) flags |= KEY_GOD;
        net_send_input(&g_ns, g_input_seq++, g_inp.yaw, g_inp.pitch, flags);
    }

    /* --- Render --- */
    if (local && local->alive) {
        renderer_set_camera(g_renderer, local);
    }

    /* One-shot diagnostic camera override, native testing only in
     * practice (no automated screenshot tooling on either target — see
     * the comment further down): the real gameplay camera's position/yaw
     * at any given frame depends on physics (free-fall to the floor) and
     * possibly server-driven state, so it can't be relied on to actually
     * be looking at the Phase 1 test MeshObject. Point the camera
     * directly at it for exactly this one frame instead, to get a
     * deterministic visibility check — next frame's renderer_set_camera
     * call above overwrites this back to the real player camera, no
     * explicit restore needed. */
    static int s_cam_diag_frame = 0;
    ++s_cam_diag_frame;
    int mesh_obj_diag_frame = g_test_mesh_loaded && s_cam_diag_frame == 30;
    if (mesh_obj_diag_frame) {
        g_renderer->cam_pos[0] = g_test_mesh_object.position.x;
        g_renderer->cam_pos[1] = g_test_mesh_object.position.y;
        g_renderer->cam_pos[2] = g_test_mesh_object.position.z + 30.0f;
        g_renderer->cam_yaw = 0.0f;
        g_renderer->cam_pitch = 0.0f;
    }

    /* [geometry]/[shadow]/[lighting]/.../[fxaa] all now happen inside the
     * Scene panel specifically (ui.c's draw_panel_scene), scoped to that
     * panel's own screen sub-rectangle rather than always filling the
     * whole window — see gbuffer_set_viewport_offset()'s comment in
     * gbuffer.h for why this needed a small Phase-0 extension. ui_render()
     * draws the branding bar and every panel (Scene included, via the
     * draw_scene_content callback below) in one call. ui_layout() already
     * ran and ui_ctx was already built above, before the click-routing
     * that needed both — reused here rather than redone. */
    ui_render(&ui_ctx);

    /* HUD */
    int bots_alive = 0;
    for (int i = 0; i < g_gs.num_players; i++)
        if (g_gs.players[i].is_bot && g_gs.players[i].alive) bots_alive++;
    update_hud(local ? local->hp : 0, bots_alive, g_ns.local_id);
    update_editor_ui(&g_ed);
    update_console_ui(&g_cs);

    /* Visual-correctness check: no automated screenshot tooling is
     * available on either target, so sample the center pixel via
     * glReadPixels periodically instead. printf reaches the browser
     * console on wasm too (Emscripten redirects stdout there), so this
     * is useful cross-platform now, not just a native-only workaround.
     *
     * The Scene panel no longer always fills the whole window (it's one
     * area in the golden-ratio default layout), so "center" now means the
     * Scene panel's own center, not the window's — ui_get_scene_rect()
     * gives its current on-screen rect (top-left origin, y-down); g_gbuf's
     * own w/h already equal the panel's size (draw_panel_scene resizes it
     * to match every frame), so object-id picks use G-buffer-local
     * coordinates directly, while the default-framebuffer readPixels below
     * needs real window pixel coordinates (accounting for the panel's
     * screen position and GL's bottom-left origin). */
    static int s_frame = 0;
    ++s_frame;
    float scene_x = 0, scene_y = 0, scene_w = 0, scene_h = 0;
    int have_scene = ui_get_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);
    if (have_scene && (s_frame == 30 || s_frame % 120 == 0)) {
        int screen_cx = (int)(scene_x + scene_w * 0.5f);
        int screen_cy_gl = ch - (int)(scene_y + scene_h * 0.5f);  /* y-down panel coord -> GL bottom-left */
        /* GL_RGBA, not GL_RGB — WebGL2's readPixels only guarantees
         * RGBA/UNSIGNED_BYTE as a legal format/type combination for an
         * arbitrary framebuffer; GL_RGB raised INVALID_OPERATION there
         * (desktop GL accepts it fine, which is why this went unnoticed
         * until an actual browser test caught it). A failed readPixels
         * doesn't write its output buffer at all, so px[] was silently
         * returning stale stack contents every time, not a real sample —
         * this is what looked like "rendering frozen" during the last
         * verification pass. */
        unsigned char px[4];
        glReadPixels(screen_cx, screen_cy_gl, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("[main] frame %d Scene panel center pixel RGB = (%d,%d,%d)\n", s_frame, px[0], px[1], px[2]);
        /* readPixels object-id selection (Phase 0 deliverable): pick the
         * center pixel's object id from the G-buffer's object_id target
         * (G-buffer-local coords -- see comment above). */
        unsigned int picked = gbuffer_pick_object_id(g_gbuf, g_gbuf->w / 2, g_gbuf->h / 2);
        printf("[main] frame %d Scene panel center pixel object_id = %u (0xFFFFFFFF = nothing drawn there)\n",
               s_frame, picked);
    }
    if (local && s_frame % 120 == 0) {
        printf("[main] frame %d pos=(%.1f,%.1f,%.1f) yaw=%.3f\n",
               s_frame, local->pos.x, local->pos.y, local->pos.z, local->yaw);
    }

    /* One-shot: confirm the Phase 1 test MeshObject (object_id 4001, see
     * g_test_mesh_object) is genuinely rasterized SOMEWHERE on screen, not
     * just loaded into memory — a coarse full-framebuffer object-id scan
     * rather than relying on the center pixel happening to land on it.
     * G-buffer-local coordinates throughout (see the comment above). */
    static int s_mesh_obj_checked = 0;
    if (have_scene && g_test_mesh_loaded && !s_mesh_obj_checked && s_frame == 30) {
        s_mesh_obj_checked = 1;
        int found = 0, min_x = -1, max_x = -1, min_y = -1, max_y = -1;
        for (int y = 0; y < g_gbuf->h; y += 4) {
            for (int x = 0; x < g_gbuf->w; x += 4) {
                unsigned int id = gbuffer_pick_object_id(g_gbuf, x, y);
                if (id == 4000u + (unsigned int)g_test_mesh_object.id) {
                    found++;
                    if (min_x < 0 || x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    if (min_y < 0 || y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                }
            }
        }
        printf("[main] MeshObject visibility scan: %d/4-stride hits, bbox x=[%d,%d] y=[%d,%d] "
               "(0 hits means not currently visible from spawn — may need camera/position adjustment)\n",
               found, min_x, max_x, min_y, max_y);
    }

    phi_platform_swap();
}

/* ---- Entry point ---- */
int main(void) {
#ifdef _WIN32
    /* Windows console stdout is fully buffered when not attached to a
     * real console (e.g. redirected to a file for headless verification)
     * — without this, output is lost whenever the process is killed
     * rather than exited normally, which it always is here since
     * phi_platform_set_main_loop() only returns on a window-close message. */
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
    printf("[main] Initialising octree world...\n");

    /* World */
    g_world = octree_create();
    octree_make_default_map(g_world);
    g_gs.world = g_world;

    /* Platform + GL context — must exist BEFORE building mesh VBOs */
    PhiPlatformConfig pcfg = { .title = "Phi", .width = 1280, .height = 720 };
    phi_platform_init(&pcfg);
    int w, h;
    phi_platform_get_window_size(&w, &h);
    g_renderer = renderer_create(w, h);
    g_gbuf = gbuffer_create(w, h);
    if (!ui_init()) printf("[main] WARNING: ui_init() failed -- editor UI will not render correctly\n");

    /* Build mesh NOW that GL context exists */
    g_mesh = mesh_create();
    mesh_rebuild(g_mesh, g_world);
    mesh_upload(g_mesh);

    /* Phase 1 foundation test object — see g_test_mesh_object's comment
     * and spawn_test_mesh_object() above (also reused by the scene
     * context menu's "Add > Mesh Object", see CTX_ACTION_ADD_MESH). */
    spawn_test_mesh_object();

    /* Add local player immediately at ID=1 so camera works before server ACKs */
    g_ns.local_id = 1;
    {
        Player *lp = &g_gs.players[0];
        memset(lp, 0, sizeof(*lp));
        lp->id    = 1;
        lp->pos   = (Vec3f){128, 48, 128};   /* y=32 sat inside the central platform's solid base — stuck-in-wall bug */
        lp->hp    = 100;
        lp->alive = 1;
        g_gs.num_players = 1;
    }

    /* Spawn bots */
    physics_spawn_bots(&g_gs, DEFAULT_BOTS);

    /* Input */
    input_init(&g_inp);
    input_install_callbacks(&g_inp);

    /* Editor / console */
    editor_init(&g_ed);
    console_init(&g_cs);

    /* Network */
    memset(&g_ns, 0, sizeof(g_ns));
    g_ns.local_id = 1;  /* will be overwritten by server HELLO */
    net_set_game_state(&g_gs);
#ifdef __EMSCRIPTEN__
    /* Build WS URL in C from location, pass as C string */
    EM_ASM({
        var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        var url   = proto + '//' + location.host + '/ws';
        var len   = lengthBytesUTF8(url) + 1;
        var buf   = _malloc(len);
        stringToUTF8(url, buf, len);
        _net_connect_js(buf);
        _free(buf);
    });
#else
    net_connect(&g_ns, "ws://localhost:8765/ws");
#endif

    g_last_t = phi_platform_now();
    phi_platform_set_main_loop(main_loop, NULL);
    phi_platform_shutdown();
    return 0;
}

/* Called from JS after URL is constructed */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void net_connect_js(const char *url) {
    net_connect(&g_ns, url);
}
#endif
