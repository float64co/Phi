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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES2/gl2.h>
#else
#include <GL/gl.h>
#endif

/* ---- Global state ---- */
static Octree      *g_world    = NULL;
static RenderMesh  *g_mesh     = NULL;
static Renderer    *g_renderer = NULL;
static GameState    g_gs       = {0};
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

/* ---- Main loop ---- */
static void main_loop(void *userdata) {
    (void)userdata;
    double now = phi_platform_now();
    float dt = (float)(now - g_last_t);
    g_last_t = now;
    if (dt > 0.05f) dt = 0.05f;   /* cap at 50ms */

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
    int cw, ch;
    phi_platform_get_window_size(&cw, &ch);
    if (cw != g_renderer->vp_w || ch != g_renderer->vp_h)
        renderer_resize(g_renderer, cw, ch);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (local && local->alive) {
        renderer_set_camera(g_renderer, local);
    }

    renderer_draw_world(g_renderer, g_mesh);
    renderer_draw_ground_plane(g_renderer);
    renderer_draw_players(g_renderer, &g_gs, g_ns.local_id);
    renderer_draw_rockets(g_renderer, &g_gs);
    editor_render(&g_ed, g_renderer);

    /* HUD */
    int bots_alive = 0;
    for (int i = 0; i < g_gs.num_players; i++)
        if (g_gs.players[i].is_bot && g_gs.players[i].alive) bots_alive++;
    update_hud(local ? local->hp : 0, bots_alive, g_ns.local_id);
    update_editor_ui(&g_ed);
    update_console_ui(&g_cs);

#ifndef __EMSCRIPTEN__
    /* One-shot visual-correctness check: no browser/screenshot tooling is
     * available for native builds, so sample the center pixel via
     * glReadPixels once rendering has had a few frames to settle instead.
     * Prints the sky-blue clear color's RGB if the world/ground-plane/sky
     * happens to not cover the exact center pixel, or a different color if
     * geometry does — either way, proof the frame isn't just black/garbage. */
    static int s_frame = 0;
    if (++s_frame == 30) {
        unsigned char px[3];
        glReadPixels(cw / 2, ch / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        printf("[main] frame %d center pixel RGB = (%d,%d,%d)\n", s_frame, px[0], px[1], px[2]);
    }
#endif

    phi_platform_swap();
}

/* ---- Entry point ---- */
int main(void) {
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

    /* Build mesh NOW that GL context exists */
    g_mesh = mesh_create();
    mesh_rebuild(g_mesh, g_world);
    mesh_upload(g_mesh);

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
