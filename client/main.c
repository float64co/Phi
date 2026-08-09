#include "octree_render.h"
#include "renderer.h"
#include "net.h"
#include "input.h"
#include "console.h"
#include "phi_platform.h"
#include "halfedge_gltf.h"
#include "meshobject.h"
#include "mesh_edit.h"
#include "fracture.h"
#include "mp_port.h"
#include "gizmo.h"
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
static Renderer    *g_renderer = NULL;
static GBuffer      *g_gbuf     = NULL;  /* deferred renderer, see gbuffer.h — both build targets now */

/* Phase 1 foundation test object: loads assets/cube.gltf (a hand-authored
 * unit cube, see the asset's own generator script) through the glTF ->
 * half-edge -> RenderMesh pipeline (halfedge_gltf.c, meshobject.c) at
 * startup and renders it as a fixed, static, non-interactive object —
 * proving the data actually flows into the live scene. No editor
 * interaction, UI, or physics yet; see phi.md's Phase 1 section. */
static MeshObject   g_test_mesh_object = {0};
static int          g_test_mesh_loaded = 0;
/* Which hem face (see meshobject_ray_pick_face) was under the cursor the
 * moment the Scene context menu was last opened with the test object
 * selected -- -1 if none/not applicable. Extrude/Inset/Loop Cut menu rows
 * (CTX_ACTION_EXTRUDE_FACE etc.) act on this. Set once at rmb_click time
 * rather than re-picked when the menu row is actually clicked, matching
 * how a real editor's context menu captures "what was under the cursor
 * when you right-clicked", not "what's under the cursor now". */
static int          g_edit_face = -1;
static Vec3f        g_edit_hit_local = {0, 0, 0};  /* local-space hit point paired with g_edit_face, see below */
static NetState     g_ns       = {0};
static InputState   g_inp      = {0};
static ConsoleState g_cs       = {0};
static double       g_last_t   = 0.0;

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
    meshobject_build_render_mesh_from_halfedge(g_test_mesh_object.render_mesh, hem);
    /* Kept alive (not halfedge_destroy'd) as the object's live editable
     * representation -- extrude/inset/loop-cut (mesh_edit.c) mutate this
     * in place and re-flatten, so it needs to survive past this one
     * initial build the way it used to only ever be used for. */
    g_test_mesh_object.hem = hem;
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
    halfedge_destroy(g_test_mesh_object.hem);
    g_test_mesh_object.hem = NULL;
    g_test_mesh_loaded = 0;
    g_edit_face = -1;
    printf("[main] deleted MeshObject\n");
}

/* Re-flattens g_test_mesh_object.hem into its render_mesh after a mesh
 * edit op mutates it (extrude/inset/loop-cut, see mesh_edit.c), and prints
 * a before/after triangle count -- the topology sanity check this loop's
 * own verification standard calls for (matching the same "real, checked
 * behavior over cosmetic-only claims" bar the ray-picking/gizmo work
 * already established this session), not just "it compiled". */
static void rebuild_test_mesh_render(const char *op_name, int tris_before) {
    meshobject_build_render_mesh_from_halfedge(g_test_mesh_object.render_mesh, g_test_mesh_object.hem);
    int tris_after = g_test_mesh_object.render_mesh->count / 3;
    printf("[main] mesh edit '%s': %d -> %d triangles\n", op_name, tris_before, tris_after);
}

/* Every renderer_draw_* call for the frame's scene content — extracted
 * into a callback (see UIRenderContext.draw_scene_content in ui.h) so the
 * Scene panel can drive it between gbuffer_begin_geometry_pass() and
 * gbuffer_render_shadow_map(). Used to also draw Qek's octree world mesh,
 * ground plane, players, and rockets, plus the octree carve-editor
 * overlay — all gone along with that code (see phi.md's Phase 1 status,
 * "Client/server model"). The MeshObject + gizmo below are the only scene
 * content that exists right now. */
static int selected_is_test_mesh(void);  /* defined below, needed here for the gizmo draw */

static void scene_content_cb(void *userdata) {
    (void)userdata;
    if (g_test_mesh_loaded) renderer_draw_mesh_object(g_renderer, &g_test_mesh_object);
    if (selected_is_test_mesh()) gizmo_draw(g_renderer, g_test_mesh_object.position);
}

/* Constructs a world-space ray from a screen point inside the Scene
 * panel's own content rect, through the same camera basis renderer.c's
 * build_vp/mat4_look_dir uses (fwd = (-sin(yaw)cos(pitch), sin(pitch),
 * -cos(yaw)cos(pitch)), right = (cos(yaw), 0, -sin(yaw)) — matched
 * exactly, not re-derived, so a constructed ray always agrees with what's
 * actually rendered). Shared by object picking and the gizmo (both
 * hit-testing and per-frame drag updates need "the ray under the current
 * mouse position" using the identical math). Returns 0 (leaving
 * origin/dir untouched) if the Scene rect is degenerate. */
static int compute_scene_ray(float scene_x, float scene_y, float scene_w, float scene_h,
                              int px, int py, Vec3f *origin, Vec3f *dir) {
    if (scene_w < 1.0f || scene_h < 1.0f) return 0;
    float ndc_x = 2.0f * ((float)px - scene_x) / scene_w - 1.0f;
    float ndc_y = 1.0f - 2.0f * ((float)py - scene_y) / scene_h;  /* screen y-down -> NDC y-up */

    float yaw = g_renderer->cam_yaw, pitch = g_renderer->cam_pitch;
    float sy = sinf(yaw),  cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    Vec3f fwd   = { -sy*cp, sp, -cy*cp };
    Vec3f right = {  cy,    0.0f, -sy   };
    Vec3f up    = {  sy*sp, cp,   cy*sp };

    float half_h = tanf(g_renderer->fov_y * 0.5f);
    float half_w = half_h * (scene_w / scene_h);
    Vec3f d = {
        fwd.x + right.x * ndc_x * half_w + up.x * ndc_y * half_h,
        fwd.y + right.y * ndc_x * half_w + up.y * ndc_y * half_h,
        fwd.z + right.z * ndc_x * half_w + up.z * ndc_y * half_h
    };
    float len = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
    if (len > 1e-6f) { d.x /= len; d.y /= len; d.z /= len; }

    dir->x = d.x; dir->y = d.y; dir->z = d.z;
    origin->x = g_renderer->cam_pos[0];
    origin->y = g_renderer->cam_pos[1];
    origin->z = g_renderer->cam_pos[2];
    return 1;
}

/* Whether the currently-selected object (via the UI's shared selection
 * state) is the one test-object slot — the gizmo only ever has this one
 * object to attach to right now, same "one slot, not a general list"
 * honesty already established for Add/Delete. */
static int selected_is_test_mesh(void) {
    return g_test_mesh_loaded &&
           ui_get_selected_object() == 4000u + (unsigned int)g_test_mesh_object.id;
}

/* Ray-vs-mesh picking, plus gizmo handle priority: if the selected
 * object's translate gizmo is showing, a click on one of its handles
 * begins a drag instead of re-picking — grabbing the gizmo must win over
 * whatever's directly behind it. Otherwise tests against
 * g_test_mesh_object's real triangle data (meshobject.c's
 * meshobject_ray_pick_face, Möller–Trumbore, not a bounding-box guess); a
 * hit selects (same 4000+id convention Outliner/gbuffer picking already
 * use), a miss deselects, matching normal editor click-away-to-deselect
 * behavior. */
static void try_pick_object(float scene_x, float scene_y, float scene_w, float scene_h, int click_x, int click_y) {
    Vec3f origin, dir;
    if (!compute_scene_ray(scene_x, scene_y, scene_w, scene_h, click_x, click_y, &origin, &dir)) return;

    if (selected_is_test_mesh()) {
        GizmoAxis axis = gizmo_pick_handle(g_test_mesh_object.position, origin, dir);
        if (axis != GIZMO_AXIS_NONE) {
            gizmo_begin_drag(axis, g_test_mesh_object.position, origin, dir);
            return;
        }
    }

    /* Face-level pick (not just whole-object) so g_edit_face -- and
     * therefore the Properties panel's material readout -- stays in sync
     * with plain left-clicks too, not only the right-click-to-open-the-
     * context-menu path (see g_edit_face's own comment). Uses hem directly
     * via meshobject_ray_pick_face rather than the coarser render_mesh-
     * only meshobject_ray_pick, since the test object always has a hem
     * once loaded (see spawn_test_mesh_object) and this gives a face index
     * for free from the same ray cast. */
    float t; int face;
    if (g_test_mesh_loaded && meshobject_ray_pick_face(&g_test_mesh_object, origin, dir, &t, &face)) {
        ui_set_selected_object(4000u + (unsigned int)g_test_mesh_object.id);
        g_edit_face = face;
    } else {
        ui_set_selected_object(0xFFFFFFFFu);
        g_edit_face = -1;
    }
}

/* ---- Main loop ---- */
static void main_loop(void *userdata) {
    (void)userdata;
    double now = phi_platform_now();
    float dt = (float)(now - g_last_t);
    g_last_t = now;
    if (dt > 0.05f) dt = 0.05f;   /* cap at 50ms */
    (void)dt;   /* no per-frame simulation reads this yet -- kept for whatever real game-state tick lands on the repurposed networking layer next */

#ifndef __EMSCRIPTEN__
    net_poll_native();  /* wasm gets messages via an async JS callback instead */
#endif

    /* --- UI layout + click routing --- ui_layout() needs to run before
     * hit-testing (it's what computes every Area's on-screen rect), and
     * UIRenderContext is built once here and reused for the ui_render()
     * call further down too — one source of truth for what got clicked
     * vs. what actually gets drawn this frame, rather than two separately-
     * constructed contexts that could drift apart. */
    int cw, ch;
    phi_platform_get_window_size(&cw, &ch);
    ui_layout(cw, ch);

    static const float light_dir[3] = {0.577f, 0.577f, 0.577f};
    float sky[3];
    renderer_get_sky_color(sky);
    UIRenderContext ui_ctx = {0};
    ui_ctx.renderer = g_renderer;
    ui_ctx.gbuf = g_gbuf;
    ui_ctx.test_obj = &g_test_mesh_object;
    ui_ctx.test_obj_loaded = g_test_mesh_loaded;
    ui_ctx.edit_face = g_edit_face;
    ui_ctx.console = &g_cs;
    memcpy(ui_ctx.light_dir, light_dir, sizeof(light_dir));
    memcpy(ui_ctx.sky_color, sky, sizeof(sky));
    ui_ctx.draw_scene_content = scene_content_cb;

    /* Real clicks (lmb_click/rmb_click) — same "rising edge, drained and
     * cleared here" convention as enter_edge etc. Routes into the panel
     * type-switcher icon/dropdown and Outliner row selection first, then
     * (if nothing claimed it) a real ray-vs-mesh pick inside the Scene
     * panel's own content (see try_pick_object) — a Scene-panel click is
     * always a select-or-deselect editorial action now (hit selects, miss
     * deselects). */
    int ui_consumed_click = 0;
    if (g_inp.lmb_click) {
        g_inp.lmb_click = 0;
        ui_consumed_click = ui_on_mouse_button(g_inp.mouse_x, g_inp.mouse_y, 0, 1, &ui_ctx);
        if (!ui_consumed_click) {
            float sx, sy, sw, sh;
            if (ui_get_scene_rect(&sx, &sy, &sw, &sh) &&
                (float)g_inp.mouse_x >= sx && (float)g_inp.mouse_x < sx + sw &&
                (float)g_inp.mouse_y >= sy && (float)g_inp.mouse_y < sy + sh) {
                try_pick_object(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y);
                ui_consumed_click = 1;
            }
        }
    }
    if (g_inp.rmb_click) {
        g_inp.rmb_click = 0;
        /* First offer the click to existing UI chrome (dismisses an
         * already-open context/type-switcher menu, same as any other
         * right-click ui_on_mouse_button already handles). If nothing
         * claimed it and the click landed inside the Scene panel's own
         * content rect (not its chrome), open the scene context menu
         * there. */
        int rmb_consumed = ui_on_mouse_button(g_inp.mouse_x, g_inp.mouse_y, 1, 1, &ui_ctx);
        if (!rmb_consumed) {
            float sx, sy, sw, sh;
            if (ui_get_scene_rect(&sx, &sy, &sw, &sh) &&
                (float)g_inp.mouse_x >= sx && (float)g_inp.mouse_x < sx + sw &&
                (float)g_inp.mouse_y >= sy && (float)g_inp.mouse_y < sy + sh) {
                ui_open_scene_context_menu(g_inp.mouse_x, g_inp.mouse_y);
                /* Capture which face (if any) is under the cursor right
                 * now, for the Extrude/Inset/Loop Cut rows this menu can
                 * show -- see g_edit_face's own comment on why this is
                 * captured here rather than re-picked at click-a-row time. */
                g_edit_face = -1;
                if (selected_is_test_mesh()) {
                    Vec3f origin, dir;
                    if (compute_scene_ray(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y, &origin, &dir)) {
                        float t; int face;
                        if (meshobject_ray_pick_face(&g_test_mesh_object, origin, dir, &t, &face)) {
                            g_edit_face = face;
                            Vec3f world_hit = { origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t };
                            g_edit_hit_local = meshobject_world_to_local(&g_test_mesh_object, world_hit);
                        }
                    }
                }
            }
        }
    }

    /* Gizmo drag: unlike the click flags above, this needs to run every
     * frame the LMB stays held (gizmo_begin_drag already ran on the press
     * that grabbed a handle, inside try_pick_object above) — a continuous
     * "where's the mouse now" update, not a one-shot edge. Ends the drag
     * the frame LMB is released; a released-but-never-dragging frame is a
     * harmless no-op (gizmo_update_drag returns 0 without touching
     * anything when nothing's being dragged). */
    if (gizmo_is_dragging()) {
        if (g_inp.lmb_down) {
            float sx, sy, sw, sh;
            if (ui_get_scene_rect(&sx, &sy, &sw, &sh)) {
                Vec3f origin, dir;
                if (compute_scene_ray(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y, &origin, &dir)) {
                    gizmo_update_drag(&g_test_mesh_object.position, origin, dir);
                }
            }
        } else {
            gizmo_end_drag();
        }
    }

    /* Scene context-menu action, drained once per frame like the click
     * flags above. Only Add Mesh Object / Delete / Extrude / Inset / Loop
     * Cut / Fracture are wired to real behavior — Frame Selected/Frame
     * All/Deselect All are still placeholder rows (see
     * ui_poll_context_menu_action's own comment). */
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
        case CTX_ACTION_EXTRUDE_FACE:
            if (g_test_mesh_loaded && g_edit_face >= 0) {
                int before = g_test_mesh_object.render_mesh->count / 3;
                /* Fixed 4-unit offset along the face's own normal -- no
                 * mouse-driven interactive extrude distance in this pass,
                 * matching this loop's "minimal, correct, not Blender-grade
                 * polish" bar (see mesh_edit.h). */
                int cap = mesh_edit_extrude_face(g_test_mesh_object.hem, g_edit_face, 4.0f);
                if (cap >= 0) {
                    rebuild_test_mesh_render("Extrude Face", before);
                    g_edit_face = cap;  /* new cap becomes the natural next target, e.g. a chained extrude */
                } else {
                    printf("[main] context menu Extrude Face: operation failed (non-triangle or stale face)\n");
                }
            } else {
                printf("[main] context menu Extrude Face: no face under the last right-click\n");
            }
            break;
        case CTX_ACTION_INSET_FACE:
            if (g_test_mesh_loaded && g_edit_face >= 0) {
                int before = g_test_mesh_object.render_mesh->count / 3;
                int cap = mesh_edit_inset_face(g_test_mesh_object.hem, g_edit_face, 0.4f);
                if (cap >= 0) {
                    rebuild_test_mesh_render("Inset Face", before);
                    g_edit_face = cap;
                } else {
                    printf("[main] context menu Inset Face: operation failed (non-triangle or stale face)\n");
                }
            } else {
                printf("[main] context menu Inset Face: no face under the last right-click\n");
            }
            break;
        case CTX_ACTION_LOOP_CUT:
            if (g_test_mesh_loaded && g_edit_face >= 0) {
                int before = g_test_mesh_object.render_mesh->count / 3;
                int edge = mesh_edit_nearest_edge_of_face(g_test_mesh_object.hem, g_edit_face,
                                                            g_edit_hit_local.x, g_edit_hit_local.y, g_edit_hit_local.z);
                int mv = edge >= 0 ? mesh_edit_loop_cut_edge(g_test_mesh_object.hem, edge) : -1;
                if (mv >= 0) {
                    rebuild_test_mesh_render("Loop Cut", before);
                } else {
                    printf("[main] context menu Loop Cut: operation failed (no edge/stale face)\n");
                }
                g_edit_face = -1;  /* the picked face was deleted by the cut, don't keep pointing at it */
            } else {
                printf("[main] context menu Loop Cut: no face under the last right-click\n");
            }
            break;
        case CTX_ACTION_FRACTURE:
            /* Editor-only precompute (see fracture.h) -- acts on the whole
             * selected MeshObject, not g_edit_face. Fixed 8-fragment count
             * and a real time-based seed (not a fixed test seed -- this is
             * the actual editor action, mesh_edit_test.c's fixed seed=42 is
             * what makes THAT reproducible/testable, this is the real
             * thing) for this pass; no interactive fragment-count picker
             * UI, matching this task's precompute-tool scope. Does NOT
             * mutate g_test_mesh_object itself or activate anything at
             * runtime -- purely writes the fragments to disk. */
            if (g_test_mesh_loaded && ui_get_selected_object() == 4000u + (unsigned int)g_test_mesh_object.id) {
                const int n_frag = 8;
                unsigned int seed = (unsigned int)phi_platform_now();
                FractureFragment *frags = fracture_voronoi(&g_test_mesh_object, n_frag, seed);
                if (frags) {
                    int ok = fracture_save_glb(frags, n_frag, "assets/fracture_output.gltf");
                    int non_empty = 0;
                    for (int i = 0; i < n_frag; i++) if (frags[i].pos_count > 0) non_empty++;
                    printf("[main] context menu Fracture: %d/%d non-empty fragments, %s -> assets/fracture_output.gltf\n",
                           non_empty, n_frag, ok ? "saved" : "SAVE FAILED");
                    fracture_free_fragments(frags, n_frag);
                } else {
                    printf("[main] context menu Fracture: operation failed (no hem?)\n");
                }
            } else {
                printf("[main] context menu Fracture: no MeshObject selected\n");
            }
            break;
        default:
            break;
    }

    /* The Python panel is an always-focused text input now (see
     * console.c) -- no Player-alive gate needed to run it, unlike the Qek
     * console this replaced. */
    console_update(&g_cs, &g_inp);

    /* --- Render --- */
    /* [geometry]/[shadow]/[lighting]/.../[fxaa] all happen inside the
     * Scene panel specifically (ui.c's draw_panel_scene), scoped to that
     * panel's own screen sub-rectangle rather than always filling the
     * whole window — see gbuffer_set_viewport_offset()'s comment in
     * gbuffer.h for why this needed a small Phase-0 extension. ui_render()
     * draws the branding bar and every panel (Scene included, via the
     * draw_scene_content callback above) in one call. ui_layout() already
     * ran and ui_ctx was already built above, before the click-routing
     * that needed both — reused here rather than redone. */
    ui_render(&ui_ctx);

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
    /* Declared here, at main()'s own top level, and never touched again --
     * its ADDRESS (not its value) is what matters, as the GC's conservative
     * stack-scan upper bound. See mp_port.h's phi_mp_init() comment for why
     * it has to be captured from exactly this scope. */
    int mp_stack_top;
#ifdef _WIN32
    /* Windows console stdout is fully buffered when not attached to a
     * real console (e.g. redirected to a file for headless verification)
     * — without this, output is lost whenever the process is killed
     * rather than exited normally, which it always is here since
     * phi_platform_set_main_loop() only returns on a window-close message. */
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
    printf("[main] Initialising Phi...\n");

    /* Platform + GL context — must exist BEFORE building mesh VBOs */
    PhiPlatformConfig pcfg = { .title = "Phi", .width = 1280, .height = 720 };
    phi_platform_init(&pcfg);
    int w, h;
    phi_platform_get_window_size(&w, &h);
    g_renderer = renderer_create(w, h);
    g_gbuf = gbuffer_create(w, h);
    if (!ui_init()) printf("[main] WARNING: ui_init() failed -- editor UI will not render correctly\n");

    /* Phase 1 foundation test object — see g_test_mesh_object's comment
     * and spawn_test_mesh_object() above (also reused by the scene
     * context menu's "Add > Mesh Object", see CTX_ACTION_ADD_MESH). */
    spawn_test_mesh_object();

    /* Fixed initial camera vantage point, close enough to the test
     * object's own spawn position to see it -- there is no real editor
     * camera navigation (orbit/pan/zoom/fly) yet, this is deliberately
     * just "look at approximately the right place on startup" rather than
     * pretending a camera-control feature exists. Previously driven by
     * Qek's Player position/mouse-look every frame; that's gone along
     * with the rest of that code (see phi.md's Phase 1 status, "Client/
     * server model"), so this is set once here and never touched again
     * unless/until real camera controls land. */
    renderer_set_camera(g_renderer, (Vec3f){128.0f, 100.0f, 120.0f}, 0.0f, 0.0f);

    /* Input */
    input_init(&g_inp);
    input_install_callbacks(&g_inp);

    /* Console / Python panel */
    console_init(&g_cs);
    phi_mp_init(&mp_stack_top);

    /* Network -- see phi.md's Phase 1 status, "Client/server model": this
     * is Qek's connection/transport machinery, repurposed rather than
     * removed, now carrying just a HELLO handshake and a generic
     * server->client text-message channel (see net.h) rather than FPS/
     * octree state sync. */
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
