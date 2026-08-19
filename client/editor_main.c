#include "octree_render.h"
#include "renderer.h"
#include "net.h"
#include "input.h"
#include "console.h"
#include "phi_platform.h"
#include "halfedge_gltf.h"
#include "skinned_mesh.h"
#include "texture_cache.h"
#include "meshobject.h"
#include "phi_physics.h"
#include "mesh_edit.h"
#include "fracture.h"
#include "mp_port.h"
#include "gizmo.h"
#include "transform_op.h"
#include "light.h"
#include "render_settings.h"
#include "scene_target.h"
#include "fracture_body.h"
#include "scene_objects.h"
#include "skinned_scene_objects.h"
#include "frame_pacer.h"
#include "node_graph.h"
#include "render_hooks.h"
#include "input_gamepad.h"
#include "path_tracer.h"
#include "skinned_mesh_object.h"
#include "ragdoll.h"
#include "font.h"
#include "svg_icon.h"
#include "asset_browser.h"
#include "chat.h"
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
/* Asset Browser Create flow (HTTP POST) -- native-Linux-only for now, see
 * http_client_native.h's own comment: no win32 (Winsock) twin or wasm
 * (fetch()) equivalent exists yet, flagged rather than silently assumed
 * to work everywhere net.c already does. */
#include "http_client_native.h"
#define PHI_HAVE_HTTP_CLIENT 1
#endif
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

/* Real multi-object scene graph (client/scene_objects.h) -- replaced the
 * single g_test_mesh_object slot every earlier phase this session built
 * around, per explicit request ("make it so the scene can have multiple
 * meshes at the same time"). Any number (up to SCENE_MAX_OBJECTS) of
 * independently loaded/created/positioned MeshObjects can now be live at
 * once -- scene_objects.c owns the actual registry; main.c drives it the
 * same way it already drives light.c's own registry (spawn/delete/find/
 * get_all, no fixed-slot assumptions anywhere new code touches). Loading
 * a mesh (Asset Browser, File > Load, Add > Mesh Object) now ADDS a new
 * object rather than replacing whatever was already there -- deliberately
 * allowed to spawn overlapping/clipping through existing geometry (no
 * collision-free placement search), separable afterward with G, per
 * explicit request. */
static void spawn_test_mesh_object(void);   /* defined below, needed here as the startup call site */

/* Whichever MeshObject the UI's shared selection currently points at, or
 * NULL if that's a Light or nothing at all -- scans scene_objects.c's
 * registry fresh each call (SCENE_MAX_OBJECTS=32 is cheap to scan; no
 * caching needed, same reasoning resolve_xform_target below already
 * relies on: selection can't change while anything that reads this is
 * mid-operation). The single source of truth for "the mesh object this
 * click/keystroke/context-menu-row is about" -- replaces the old fixed
 * g_test_mesh_object pointer everywhere one was assumed. */
static MeshObject *selected_mesh_object(void) {
    unsigned int sel = ui_get_selected_object();
    if (sel < 4000u || sel >= 5000u) return NULL;   /* MeshObjects=4000+id, see ui.h's own range comment */
    return scene_object_find((int)(sel - 4000u));
}

/* True once fracture_body_activate has produced at least one live
 * fragment (see fracture_body.h) FROM g_fracture_source_id specifically
 * -- fracture still only ever affects ONE object at a time (fracture_
 * body.c's own registry is a separate, smaller, bounded thing, not
 * generalized to N simultaneous fracture sources this pass, a real,
 * deliberate scope limit matching that module's own established scope).
 * The SOURCE object itself still exists (its hem/render_mesh untouched)
 * but stops being drawn/picked while its fragments are what's actually
 * simulated/visible -- every OTHER live scene object is completely
 * unaffected by one object being fractured. Cleared back to 0 whenever
 * the source object specifically is deleted -- there's no "un-shatter"
 * action, fracture is one-way once activated. */
static int g_fracture_active = 0;
static int g_fracture_source_id = -1;
/* Which object CTX_ACTION_SAVE_AS_ASSET/TOP_ACTION_FILE_SAVE most
 * recently began a create-flow for, by id -- the actual upload happens
 * later, once the Asset Browser's edit form is submitted (g_ab.
 * create_requested), by which point selection could have changed;
 * remembered by id (re-resolved via scene_object_find at upload time,
 * not a raw pointer -- the object could in principle be deleted in
 * between) rather than assuming "whatever's selected now" is still the
 * same thing that was being saved. -1 = nothing pending. */
static int g_save_as_asset_object_id = -1;
/* Which hem face (see meshobject_ray_pick_face) was under the cursor the
 * moment the Scene context menu was last opened with the test object
 * selected -- -1 if none/not applicable. Extrude/Inset/Loop Cut menu rows
 * (CTX_ACTION_EXTRUDE_FACE etc.) act on this. Set once at rmb_click time
 * rather than re-picked when the menu row is actually clicked, matching
 * how a real editor's context menu captures "what was under the cursor
 * when you right-clicked", not "what's under the cursor now". */
static int          g_edit_face = -1;
static Vec3f        g_edit_hit_local = {0, 0, 0};  /* local-space hit point paired with g_edit_face, see below */
/* Blender-style Object/Edit mode (see ui.h's EditorMode) -- Tab toggles it
 * (see main_loop's g_inp.tab_edge handling) and the Scene context menu's
 * "Enter/Exit Edit Mode" row does the same thing via toggle_editor_mode(),
 * one shared place rather than duplicating the entry/exit rules. g_edit_face
 * is meaningful ONLY while in Edit mode -- every Object-mode pick clears it
 * (see try_pick_object), and toggle_editor_mode() clears it on every
 * transition too, so it can never carry a stale face across a mode switch. */
static EditorMode   g_editor_mode = EDITOR_MODE_OBJECT;
/* Offline-raytracer render settings (Phase 3, see phi.md and
 * render_settings.h) -- just sample count so far. Owned here like every
 * other piece of scene state; registered with the DNA/RNA property
 * system (scene_target_register below) so the Properties panel and
 * phi.prop_get/set("render", ...) both read/write this exact struct. */
static RenderSettings g_render_settings = { 128 };
/* Phase 4's GPU-skinning + Timeline demonstration object (see skinned_
 * mesh_object.h/renderer.h's renderer_draw_skinned_mesh) -- a SEPARATE,
 * dedicated slot from g_test_mesh_object, same "own small bounded thing
 * alongside the one general test-object slot" pattern light.c/fracture_
 * body.c already established, not a rework of the halfedge/PBR-material
 * MeshObject pipeline that entity type is structurally incompatible
 * with (SkinnedVertex carries bone indices/weights, no per-face
 * material). Loaded once at startup from the same real fixture animation_
 * test/ragdoll_test/skinned_mesh_object_test already verify (assets/
 * test/armature_test.gltf) -- not user-spawnable this pass (no context-
 * menu "Add > Skinned Character" row), a real, honest scope limit. */
static SkinnedMeshObject g_skinned_test_obj = {0};
static int                g_skinned_test_loaded = 0;
#define SKINNED_TEST_ID_BASE 6000u   /* alongside MeshObjects=4000+id, Lights=5000+id, see ui.h's own comment on this convention */
static NetState     g_ns       = {0};
static InputState   g_inp      = {0};
static PyConsoleState g_cs     = {0};
static AssetBrowserState g_ab  = {0};
static ChatState    g_chat     = {0};
static double       g_last_t   = 0.0;

/* Editor camera -- a real orbit camera (pivot + distance + yaw/pitch),
 * not the plain eye-position+yaw/pitch fly camera this used to be:
 * Blender-style navigation (MMB-drag orbit, Ctrl+MMB-drag pan, wheel
 * zoom -- see cam_recompute_pos/scene_zoom_cb/the MMB-drag block in
 * main_loop) needs a fixed point to orbit AROUND, which a bare eye-
 * position has nowhere to keep. g_cam_pos is DERIVED from the other
 * four (see cam_recompute_pos), never written to directly outside it.
 * Initial values chosen so frame 1's eye position matches exactly what
 * this used to be hardcoded to -- (128,100,120) looking toward -Z at
 * yaw=pitch=0 -- so landing this was not a visible camera jump:
 * pivot = eye + forward*distance with forward=(0,0,-1) at yaw=pitch=0,
 * distance=40 -> pivot=(128,100,80). */
static Vec3f  g_cam_pivot    = {128.0f, 100.0f, 80.0f};
static float  g_cam_distance = 40.0f;
static float  g_cam_yaw      = 0.0f;
static float  g_cam_pitch    = 0.0f;
static Vec3f  g_cam_pos      = {128.0f, 100.0f, 120.0f};

/* Blender-style MMB-drag camera navigation state -- same "recompute from
 * the absolute mouse position every frame relative to where the drag
 * STARTED" pattern ui.c's area-border resize drag and the transform
 * gizmo's own drag already established (not per-frame delta
 * accumulation, so a missed mouse-move event can't cause drift). Orbit
 * vs. pan is decided once, from whether Ctrl was held at the exact
 * moment the drag started -- matches Blender's own "modifier state at
 * click time, not live-toggled mid-drag" behavior. */
static int   g_cam_dragging = 0;
static int   g_cam_drag_is_pan = 0;
static int   g_cam_drag_start_x = 0, g_cam_drag_start_y = 0;
static float g_cam_drag_start_yaw = 0.0f, g_cam_drag_start_pitch = 0.0f;
static Vec3f g_cam_drag_start_pivot = {0.0f, 0.0f, 0.0f};

/* Phase 2 physics (see phi.md's "Bullet Physics via Emscripten") -- one
 * shared world, stepped every frame in main_loop. g_ground_phys_body is a
 * single large static (mass=0) box acting as a floor so "Enable Physics"
 * (see the context-menu handler below) has something to actually land
 * on -- there's no other scene geometry to collide with since the octree
 * world was removed (see phi.md's Phase 1 "Client/server model"), and
 * building a real "author collision geometry" workflow is its own
 * separate scope, not part of this pass. */
static PhiPhysicsWorld *g_phys_world      = NULL;
static PhiRigidBody    *g_ground_phys_body = NULL;

/* Loads any glTF/GLB file as a NEW scene object (scene_objects.h) at
 * `position` -- generalized from the old spawn_test_mesh_object()'s
 * inline body, which used to overwrite the one fixed slot; now every
 * call ADDS, real multi-object support, per explicit request. `scale`
 * bakes a uniform scale directly into the flattened vertex positions (no
 * scale field on MeshObject yet, see meshobject.h) -- 16.0 for
 * cube.gltf (a unit, half-extent-0.5 cube that needs scaling up to read
 * at the scene's existing proportions), 1.0 for assets authored at that
 * scale already (see tools/gen_test_assets.py). Deliberately does NOT
 * check for/avoid overlapping existing geometry -- allowed to clip
 * through whatever's already there (per explicit request), separable
 * afterward with G (see resolve_xform_target/selected_mesh_object).
 * Returns the new object (already selected, so it's immediately
 * grabbable) on success, NULL if the file couldn't be loaded or the
 * scene registry is already full (SCENE_MAX_OBJECTS). */
static MeshObject *add_mesh_object_from_path(const char *path, float scale, Vec3f position) {
    HalfEdgeMesh *hem = halfedge_load_gltf(path);
    if (!hem) {
        printf("[main] failed to load %s — MeshObject load skipped\n", path);
        return NULL;
    }
    MeshObject *obj = scene_object_add();
    if (!obj) {
        printf("[main] failed to load %s — scene is full (SCENE_MAX_OBJECTS)\n", path);
        halfedge_destroy(hem);
        return NULL;
    }
    obj->position = position;
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj->is_static = 1;
    obj->render_mesh = mesh_create();
    if (scale != 1.0f) {
        for (int i = 0; i < hem->vert_count; i++)
            for (int a = 0; a < 3; a++)
                hem->verts[i].pos[a] *= scale;
    }
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    /* Kept alive (not halfedge_destroy'd) as the object's live editable
     * representation -- extrude/inset/loop-cut (mesh_edit.c) mutate this
     * in place and re-flatten, so it needs to survive past this one
     * initial build the way it used to only ever be used for. */
    obj->hem = hem;
    ui_set_selected_object(4000u + (unsigned int)obj->id);
    printf("[main] loaded %s as MeshObject #%d: %d triangles\n",
           path, obj->id, obj->render_mesh->count / 3);
    return obj;
}

/* Loads assets/cube.gltf as a new scene object at the scene's original
 * startup position -- factored out so the scene right-click context
 * menu's "Add > Mesh Object" (see CTX_ACTION_ADD_MESH below) can reuse
 * the same glTF -> half-edge -> RenderMesh pipeline call (that row uses
 * g_cam_pivot instead, the same "spawn at the camera's own orbit pivot"
 * convention Add Light already established, so a deliberately-added
 * object lands somewhere the user's actually looking at). */
static void spawn_test_mesh_object(void) {
    add_mesh_object_from_path("assets/cube.gltf", 16.0f, (Vec3f){128.0f, 100.0f, 90.0f});
}

/* Frees obj's GPU-side mesh/physics body and removes it from the scene
 * registry (scene_objects.h). Clears the UI's own selection first if it
 * was pointing at this object, so Properties doesn't keep showing a
 * readout for something that no longer exists. Deleting the fracture
 * SOURCE object also clears whatever it was shattered into (fracture_
 * body.h) -- there's no meaning to keeping a fragment swarm alive once
 * the object it came from is gone; deleting any OTHER object leaves an
 * active fracture completely untouched (see g_fracture_source_id). */
static void delete_mesh_object(MeshObject *obj) {
    if (!obj) return;
    if (ui_get_selected_object() == 4000u + (unsigned int)obj->id) {
        ui_set_selected_object(0xFFFFFFFFu);
    }
    if (g_fracture_active && g_fracture_source_id == obj->id) {
        fracture_body_clear(g_phys_world);
        g_fracture_active = 0;
        g_fracture_source_id = -1;
    }
    int id = obj->id;
    scene_object_delete(obj, g_phys_world);
    g_edit_face = -1;
    printf("[main] deleted MeshObject #%d\n", id);
}

/* Re-flattens obj->hem into its render_mesh after a mesh edit op mutates
 * it (extrude/inset/loop-cut, see mesh_edit.c), and prints a before/after
 * triangle count -- the topology sanity check this loop's own
 * verification standard calls for (matching the same "real, checked
 * behavior over cosmetic-only claims" bar the ray-picking/gizmo work
 * already established this session), not just "it compiled". */
static void rebuild_mesh_render(MeshObject *obj, const char *op_name, int tris_before) {
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    int tris_after = obj->render_mesh->count / 3;
    printf("[main] mesh edit '%s': %d -> %d triangles\n", op_name, tris_before, tris_after);
}

/* Every renderer_draw_* call for the frame's scene content — extracted
 * into a callback (see UIRenderContext.draw_scene_content in ui.h) so the
 * Scene panel can drive it between gbuffer_begin_geometry_pass() and
 * gbuffer_render_shadow_map(). Used to also draw Qek's octree world mesh,
 * ground plane, players, and rockets, plus the octree carve-editor
 * overlay — all gone along with that code (see phi.md's Phase 1 status,
 * "Client/server model"). Every live MeshObject (scene_objects.h) + the
 * gizmo are the scene content that exists now. */

/* Ground-aligned reference grid -- centered/sized to match
 * g_ground_phys_body exactly (position (128,40,90), half-extents
 * (200,10,200), so its top surface -- and this grid -- sit at y=50), so
 * the one visual reference plane in the scene is the same plane the test
 * object actually lands on, not an arbitrary unrelated y=0. Always drawn
 * (no toggle) -- a plain "is there a ground reference" ask, not a
 * feature that needs to be turned off. */
static void draw_scene_grid(void) {
    /* Thin + light grey per an explicit request. Width is a real world-
     * space quad (see renderer_draw_grid's own comment on why: solid
     * geometry, not GL_LINES, survives TAA/FXAA where a literal 1px line
     * doesn't -- a real, previously-hit bug in this codebase, not a
     * hypothetical one), shrunk to 0.05 (6x thinner than the original
     * 0.3) rather than switched to an actual 1px GL_LINES draw, which
     * would reintroduce that exact bug -- a hairline via solid geometry,
     * not a literal device-pixel line. */
    renderer_draw_grid(g_renderer, (Vec3f){128.0f, 50.0f, 90.0f},
                        200.0f, 10.0f, 0.05f, 0.65f, 0.65f, 0.65f);
}

static void scene_content_cb(void *userdata) {
    (void)userdata;
    draw_scene_grid();
    /* Every live scene object -- skipping only the current fracture
     * SOURCE (once fractured, its fragments, drawn below via fracture_
     * body_sync_and_draw_all, ARE that one object visually; every OTHER
     * object draws completely normally regardless of fracture state,
     * see g_fracture_source_id's own comment). */
    MeshObject *objects[SCENE_MAX_OBJECTS];
    int n_objects = scene_object_get_all(objects);
    for (int i = 0; i < n_objects; i++) {
        if (g_fracture_active && objects[i]->id == g_fracture_source_id) continue;
        renderer_draw_mesh_object(g_renderer, objects[i]);
    }
    MeshObject *sel = selected_mesh_object();
    if (sel && !(g_fracture_active && sel->id == g_fracture_source_id)) {
        gizmo_draw(g_renderer, sel->position);
    }
    renderer_draw_lights(g_renderer);
    fracture_body_sync_and_draw_all(g_renderer);
    if (g_skinned_test_loaded) renderer_draw_skinned_mesh(g_renderer, &g_skinned_test_obj, SKINNED_TEST_ID_BASE + 1u);

    /* Every live skinned scene object (skinned_scene_objects.h) -- the
     * registry-based twin of the MeshObject loop above, added 2026-08-19
     * so game/src/ *.c code (or any future editor feature) that spawns a
     * real animated character gets it drawn automatically, the same way
     * a MeshObject already does, rather than needing its own one-off
     * hardcoded slot the way g_skinned_test_obj above still is. Object-id
     * range 7000+id, alongside MeshObjects' 4000+id/Lights' 5000+id/the
     * skinned test slot's own SKINNED_TEST_ID_BASE (see ui.h's own
     * comment on this convention). */
    {
        SkinnedMeshObject *skinned_objs[SKINNED_SCENE_MAX_OBJECTS];
        int n_skinned = skinned_scene_object_get_all(skinned_objs);
        for (int i = 0; i < n_skinned; i++) {
            renderer_draw_skinned_mesh(g_renderer, skinned_objs[i],
                                        7000u + (unsigned int)skinned_scene_object_id(skinned_objs[i]));
        }
    }
}

/* Fixed output resolution for Phase 3's offline path tracer (see path_
 * tracer.h) -- only samples was ever asked to be user-configurable (via
 * the Properties panel / phi.prop_set("render", "samples", N), see
 * g_render_settings below); resolution stays a real, honest constant
 * for this pass rather than inventing an unrequested UI control for it. */
#define PT_RENDER_WIDTH  640
#define PT_RENDER_HEIGHT 480

/* File > Render Still Frame (see ui.h's TOP_ACTION_FILE_RENDER) and
 * phi.render() (mp_port.c) both funnel through here -- builds a PTScene
 * from whatever's actually visible right now (the live test object, or
 * its fracture fragments once activated, the SAME set scene_content_cb
 * above draws) and every live Light, renders one still frame from the
 * Scene panel's own current camera (g_cam_pos/g_cam_yaw/g_cam_pitch, the
 * exact state cam_recompute_pos below already maintains) at g_render_
 * settings.samples, and writes a real PNG to disk. Synchronous -- this
 * pass has no background-thread/progress-bar UI, so the editor visibly
 * hangs for the render's duration; a real, honestly flagged scope limit
 * (fine for a still frame at modest sample counts, not fine for
 * anything longer). Returns 1 and fills out_path on success, 0 on
 * failure (nothing in the scene to render, or pt_render/pt_write_png
 * themselves failing) -- printed to stdout either way so both the top-
 * menu action and the Python binding get real user-visible feedback. */
static int render_still_frame_to_disk(char *out_path, size_t out_path_cap) {
    const MeshObject *objs[SCENE_MAX_OBJECTS + PHI_MAX_FRACTURE_BODIES];
    int n_objs = 0;
    MeshObject *scene_objs[SCENE_MAX_OBJECTS];
    int n_scene = scene_object_get_all(scene_objs);
    for (int i = 0; i < n_scene && n_objs < (int)(sizeof(objs) / sizeof(objs[0])); i++) {
        if (g_fracture_active && scene_objs[i]->id == g_fracture_source_id) continue;   /* replaced by its fragments below */
        objs[n_objs++] = scene_objs[i];
    }
    if (g_fracture_active) {
        int fc = fracture_body_count();
        for (int i = 0; i < fc && n_objs < (int)(sizeof(objs) / sizeof(objs[0])); i++) {
            const MeshObject *o = fracture_body_get_object(i);
            if (o) objs[n_objs++] = o;
        }
    }

    PhiLight *lights[PHI_MAX_LIGHTS];
    int n_lights = light_get_all(lights);

    PTScene scene = pt_scene_build(objs, n_objs);
    if (scene.tri_count == 0) {
        printf("[main] Render Still Frame: scene is empty, nothing to render\n");
        pt_scene_destroy(&scene);
        return 0;
    }
    int tri_count = scene.tri_count;

    PTRenderParams params = { PT_RENDER_WIDTH, PT_RENDER_HEIGHT, g_render_settings.samples, 6 };
    float *img = pt_render(&scene, lights, n_lights, g_cam_pos, g_cam_yaw, g_cam_pitch, g_renderer->fov_y, &params);
    pt_scene_destroy(&scene);
    if (!img) {
        printf("[main] Render Still Frame: pt_render failed (invalid render settings?)\n");
        return 0;
    }

    char path[256];
    snprintf(path, sizeof(path), "assets/render_%u.png", (unsigned int)phi_platform_now());
    int ok = pt_write_png(path, img, params.width, params.height);
    free(img);
    printf("[main] Render Still Frame: %s -- %s (samples=%d, %d light(s), %d triangles)\n",
           ok ? path : "FAILED to write PNG", ok ? "done" : "", params.samples, n_lights, tri_count);
    if (ok && out_path) snprintf(out_path, out_path_cap, "%s", path);
    return ok;
}

/* phi.activate_ragdoll()'s real callback (see mp_port.h's phi_mp_
 * register_ragdoll_callback) -- activates a ragdoll from the skinned
 * test object's CURRENT pose (whatever skinned_mesh_object_update last
 * computed this frame, mid-animation or not), same "real capability,
 * triggerable from Python" pattern render_still_frame_to_disk above
 * already established for phi.render(). Returns 0 (no-op) if the
 * skinned test object never loaded. */
static int activate_ragdoll_on_test_object(void) {
    if (!g_skinned_test_loaded) return 0;
    int spawned = ragdoll_activate(g_phys_world, &g_skinned_test_obj, 1.0f, 0.2f);
    printf("[main] phi.activate_ragdoll(): spawned %d capsule bodies\n", spawned);
    return spawned;
}

/* phi.play_animation/pause_animation/resume_animation/get_animation_time/
 * set_animation_time/list_animation_clips' real callbacks (see mp_port.h's
 * phi_mp_register_animation_callbacks) -- operate on g_skinned_test_obj,
 * the same single global slot activate_ragdoll_on_test_object above
 * already targets. All six return a real 0/failure sentinel rather than
 * raising themselves -- mp_port.c's native_* wrappers decide whether that
 * becomes a Python ValueError, same division of labor as
 * render_still_frame_to_disk/activate_ragdoll_on_test_object already
 * establish. */
static int anim_play_clip(const char *clip_name, int loop) {
    if (!g_skinned_test_loaded) return 0;
    const AnimClip *clip = NULL;
    if (clip_name && clip_name[0]) {
        for (int i = 0; i < g_skinned_test_obj.clip_count; i++) {
            if (strcmp(g_skinned_test_obj.clips[i].name, clip_name) == 0) {
                clip = &g_skinned_test_obj.clips[i];
                break;
            }
        }
        if (!clip) return 0;
    } else if (g_skinned_test_obj.clip_count > 0) {
        clip = &g_skinned_test_obj.clips[0];
    } else {
        return 0;
    }
    anim_playback_play(&g_skinned_test_obj.playback, clip, loop);
    return 1;
}

static void anim_set_playing_state(int playing) {
    if (g_skinned_test_loaded && g_skinned_test_obj.playback.clip) {
        g_skinned_test_obj.playback.playing = playing;
    }
}

static int anim_get_playing_state(void) {
    return g_skinned_test_loaded && g_skinned_test_obj.playback.playing;
}

static float anim_get_playback_time(void) {
    return g_skinned_test_loaded ? g_skinned_test_obj.playback.time : 0.0f;
}

static int anim_set_playback_time(float t) {
    if (!g_skinned_test_loaded || !g_skinned_test_obj.playback.clip) return 0;
    g_skinned_test_obj.playback.time = t;
    return 1;
}

static int anim_list_clips(char out_names[][64], float *out_durations, int max_clips) {
    if (!g_skinned_test_loaded) return 0;
    int n = g_skinned_test_obj.clip_count;
    if (n > max_clips) n = max_clips;
    for (int i = 0; i < n; i++) {
        strncpy(out_names[i], g_skinned_test_obj.clips[i].name, 63);
        out_names[i][63] = 0;
        out_durations[i] = g_skinned_test_obj.clips[i].duration;
    }
    return n;
}

/* fwd/right/up basis matched EXACTLY against compute_scene_ray/
 * renderer.c's build_vp/mat4_look_dir -- every camera-navigation
 * function below (zoom/orbit/pan) reuses this same one, so none of them
 * can silently drift from what's actually rendered or from each other. */
static void cam_basis(float yaw, float pitch, Vec3f *fwd, Vec3f *right, Vec3f *up) {
    float sy = sinf(yaw),   cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    if (fwd)   *fwd   = (Vec3f){ -sy*cp, sp, -cy*cp };
    if (right) *right = (Vec3f){  cy,    0.0f, -sy   };
    if (up)    *up    = (Vec3f){  sy*sp, cp,   cy*sp };
}

/* Recomputes g_cam_pos from pivot/distance/yaw/pitch and pushes it to
 * the renderer -- the ONE place any of those four ever actually reaches
 * the renderer, so zoom/orbit/pan can each just mutate their own piece
 * of state and call this rather than duplicating the eye = pivot -
 * forward*distance math and the renderer_set_camera call three times. */
static void cam_recompute_pos(void) {
    Vec3f fwd; cam_basis(g_cam_yaw, g_cam_pitch, &fwd, NULL, NULL);
    g_cam_pos.x = g_cam_pivot.x - fwd.x * g_cam_distance;
    g_cam_pos.y = g_cam_pivot.y - fwd.y * g_cam_distance;
    g_cam_pos.z = g_cam_pivot.z - fwd.z * g_cam_distance;
    renderer_set_camera(g_renderer, g_cam_pos, g_cam_yaw, g_cam_pitch);
}

/* Mouse-wheel-over-the-Scene-panel zoom (ui.c's UIRenderContext::
 * on_scene_zoom, routed by hover, see ui_on_mouse_wheel) -- now moves
 * g_cam_distance (how far the eye sits from the orbit pivot) rather than
 * freely dollying the eye position itself, so zoom stays consistent with
 * MMB-orbit's own fixed-pivot model instead of letting the pivot drift
 * away from what's actually on screen. Clamped to a sane range: never
 * lets distance collapse to (or cross) zero, never lets it run off to an
 * unusable extreme either. */
static void scene_zoom_cb(int delta) {
    g_cam_distance -= 8.0f * (float)delta;   /* world units per wheel notch -- tuned against this scene's own scale (test cube edge 16, ground plane 400x400) */
    if (g_cam_distance < 5.0f) g_cam_distance = 5.0f;
    if (g_cam_distance > 2000.0f) g_cam_distance = 2000.0f;
    cam_recompute_pos();
}

/* Blender-style MMB-drag orbit -- yaw/pitch computed fresh from the
 * drag's START values + total pixel delta since then (never
 * accumulated incrementally frame to frame, see g_cam_dragging's own
 * comment on why). Pitch is clamped to +/-~89 degrees to avoid a gimbal
 * flip through the poles, where yaw becomes ill-defined. */
static void cam_orbit_from_start(int dx, int dy) {
    const float SENS = 0.008f;          /* radians per pixel of drag */
    const float PITCH_LIMIT = 1.55334f; /* ~89 degrees in radians */
    /* Horizontal drag direction flipped per an explicit request (was
     * +dx, now -dx) -- same "confirmed working, just inverted from the
     * feel that was actually wanted" situation as the vertical flip
     * just below. */
    g_cam_yaw = g_cam_drag_start_yaw - (float)dx * SENS;
    /* Vertical drag direction flipped per an explicit request (was +dy,
     * now -dy) -- confirmed working orbit (see the earlier "MMB orbit
     * works" report), just inverted from the feel that was actually
     * wanted. */
    g_cam_pitch = g_cam_drag_start_pitch - (float)dy * SENS;
    if (g_cam_pitch > PITCH_LIMIT) g_cam_pitch = PITCH_LIMIT;
    if (g_cam_pitch < -PITCH_LIMIT) g_cam_pitch = -PITCH_LIMIT;
    cam_recompute_pos();
}

/* Blender-style Ctrl+MMB-drag pan -- moves the pivot (and so the eye,
 * via cam_recompute_pos) across the camera's own right/up plane, using
 * the basis AT DRAG START (yaw/pitch don't change during a pan, so
 * start == current anyway; using start keeps this consistent with
 * cam_orbit_from_start's own "everything from the drag's start state"
 * shape). Screen-right drag moves the pivot along -right and screen-down
 * drag moves it along +up, so a dragged point visually stays under the
 * cursor (the standard "grab and drag the world" pan feel every 3D tool
 * uses) -- not verified live in this sandbox (XOpenDisplay(), same
 * limitation as every other interactive gesture this session), so this
 * sign convention is derived from the camera-space math, not eyeballed
 * against a real window. Pan speed scales with g_cam_distance so it
 * feels consistent whether zoomed in close or far out, matching
 * Blender's own zoom-relative pan speed. */
static void cam_pan_from_start(int dx, int dy) {
    Vec3f right, up;
    cam_basis(g_cam_drag_start_yaw, g_cam_drag_start_pitch, NULL, &right, &up);
    float scale = g_cam_distance * 0.0015f;
    float rx = -right.x * (float)dx + up.x * (float)dy;
    float ry = -right.y * (float)dx + up.y * (float)dy;
    float rz = -right.z * (float)dx + up.z * (float)dy;
    g_cam_pivot.x = g_cam_drag_start_pivot.x + rx * scale;
    g_cam_pivot.y = g_cam_drag_start_pivot.y + ry * scale;
    g_cam_pivot.z = g_cam_drag_start_pivot.z + rz * scale;
    cam_recompute_pos();
}

/* Constructs a world-space ray from a screen point inside the Scene
 * panel's own content rect, through cam_basis (the same camera basis
 * renderer.c's build_vp/mat4_look_dir uses — shared, not re-derived, so
 * a constructed ray can never drift from what's actually rendered or
 * from the camera-navigation functions above). Shared by object picking
 * and the gizmo (both hit-testing and per-frame drag updates need "the
 * ray under the current mouse position" using the identical math).
 * Returns 0 (leaving origin/dir untouched) if the Scene rect is
 * degenerate. */
static int compute_scene_ray(float scene_x, float scene_y, float scene_w, float scene_h,
                              int px, int py, Vec3f *origin, Vec3f *dir) {
    if (scene_w < 1.0f || scene_h < 1.0f) return 0;
    float ndc_x = 2.0f * ((float)px - scene_x) / scene_w - 1.0f;
    float ndc_y = 1.0f - 2.0f * ((float)py - scene_y) / scene_h;  /* screen y-down -> NDC y-up */

    Vec3f fwd, right, up;
    cam_basis(g_renderer->cam_yaw, g_renderer->cam_pitch, &fwd, &right, &up);

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

/* Light objects share the same ui_get_selected_object() selection slot
 * MeshObjects use, in a separate id range (5000+id vs. MeshObjects'
 * 4000+id -- see ui.h's own comment on the convention) so there's still
 * only ever one "currently selected thing" in the whole editor, same as
 * before lights existed. Returns the real PhiLight* (not just a bool)
 * since most callers need more than yes/no -- Properties panel, Delete,
 * and prop-set all need the actual light to act on. NULL both when the
 * selection isn't in the light range at all AND when it is but that
 * light was since deleted (light_find's own NULL-on-miss, not a second
 * check here). */
static PhiLight *selected_light(void) {
    unsigned int sel = ui_get_selected_object();
    if (sel < LIGHT_ID_BASE || sel >= LIGHT_ID_BASE + 100000u) return NULL;
    return light_find((int)(sel - LIGHT_ID_BASE));
}

/* Resolves the G/S/R transform tool's target for whatever's currently
 * selected -- the one MeshObject slot (any of Grab/Scale/Rotate, its
 * real position/orientation/scale fields) or a selected Light (Grab
 * only: a light has a real position but no orientation/scale field to
 * speak of, so the orientation/scale out-pointers are left NULL and
 * transform_op.c's own defensive guards make Scale/Rotate a safe no-op if ever
 * reached for one anyway -- the real gate is the entry-point call site
 * below, which never offers S/R for a light in the first place). Called
 * fresh every frame (begin/update/every cancel site) rather than cached,
 * since selection cannot change while a modal op is active (nothing else
 * accepts a pick during that time) -- so this always resolves to the
 * exact same target for the lifetime of one operation. Returns 1 with
 * the three out-pointers set if there's a valid target for `kind`, 0
 * (pointers untouched) otherwise. */
static int resolve_xform_target(TransformOpKind kind, Vec3f **out_position, Quat **out_orientation, Vec3f **out_scale) {
    MeshObject *obj = selected_mesh_object();
    if (obj) {
        *out_position = &obj->position;
        *out_orientation = &obj->orientation;
        *out_scale = &obj->scale;
        return 1;
    }
    PhiLight *l = selected_light();
    if (l && kind == XFORM_GRAB) {
        *out_position = &l->position;
        *out_orientation = NULL;
        *out_scale = NULL;
        return 1;
    }
    return 0;
}

/* Shared by both the Tab-key shortcut and the Scene context menu's Enter/
 * Exit Edit Mode row -- Blender only allows entering Edit mode with a mesh
 * object selected (a no-op, not silently ignored -- see the printf), and
 * exiting always succeeds. g_edit_face is Edit-mode-only state (see its own
 * comment) so it's cleared on every transition, not just Object-mode picks. */
static void toggle_editor_mode(void) {
    if (g_editor_mode == EDITOR_MODE_OBJECT) {
        if (!selected_mesh_object()) {
            printf("[main] Enter Edit Mode: no mesh object selected\n");
            return;
        }
        g_editor_mode = EDITOR_MODE_EDIT;
    } else {
        g_editor_mode = EDITOR_MODE_OBJECT;
    }
    g_edit_face = -1;
}

/* Ray-vs-mesh picking, plus gizmo handle priority: if the selected
 * object's translate gizmo is showing, a click on one of its handles
 * begins a drag instead of re-picking — grabbing the gizmo must win over
 * whatever's directly behind it. Otherwise tests EVERY live scene object's
 * real triangle data (meshobject.c's meshobject_ray_pick_face, Möller–
 * Trumbore, not a bounding-box guess), nearest hit wins; a hit selects
 * (same 4000+id convention Outliner/gbuffer picking already use), a miss
 * deselects, matching normal editor click-away-to-deselect behavior. */
static void try_pick_object(float scene_x, float scene_y, float scene_w, float scene_h, int click_x, int click_y) {
    Vec3f origin, dir;
    if (!compute_scene_ray(scene_x, scene_y, scene_w, scene_h, click_x, click_y, &origin, &dir)) return;

    if (g_editor_mode == EDITOR_MODE_EDIT) {
        /* Edit mode: restricted to face-selection within the already-
         * selected MeshObject -- no gizmo handles, no switching which
         * object is selected, matching Blender's own "Edit Mode edits one
         * object" invariant. A miss just clears the face selection, same
         * as clicking empty space in Blender's edit mode (does NOT kick
         * back to Object mode or deselect the object). */
        MeshObject *sel = selected_mesh_object();
        float t; int face;
        if (sel && !(g_fracture_active && sel->id == g_fracture_source_id) &&
            meshobject_ray_pick_face(sel, origin, dir, &t, &face)) {
            g_edit_face = face;
        } else {
            g_edit_face = -1;
        }
        return;
    }

    /* Object mode: gizmo handle priority, then whole-object select/
     * deselect -- no face-level state (g_edit_face is Edit-mode-only from
     * here on, see its own comment). Once fractured, the source object's
     * own hem/render_mesh still exist (see g_fracture_source_id's own
     * comment) but are no longer meant to be pickable -- its fragments
     * aren't first-class pickable objects either (see fracture_body.h's
     * own scope note), so a click where the shattered object used to be
     * simply misses/deselects, same as clicking any other empty space. */
    MeshObject *cur_sel = selected_mesh_object();
    if (cur_sel && !(g_fracture_active && cur_sel->id == g_fracture_source_id)) {
        GizmoAxis axis = gizmo_pick_handle(cur_sel->position, origin, dir);
        if (axis != GIZMO_AXIS_NONE) {
            gizmo_begin_drag(axis, cur_sel->position, origin, dir);
            return;
        }
    }

    /* Whole-object select: tests every live MeshObject and every live
     * light, nearest hit wins (compares real ray t, same "nearest hit
     * wins" contract meshobject_ray_pick_face and light_ray_pick both
     * already follow) -- a light or object in front of others from this
     * angle should win the pick, not "objects always checked first" or
     * "first object in the registry always wins". */
    MeshObject *objects[SCENE_MAX_OBJECTS];
    int n_objects = scene_object_get_all(objects);
    MeshObject *best_obj = NULL;
    float best_t = 0.0f;
    for (int i = 0; i < n_objects; i++) {
        if (g_fracture_active && objects[i]->id == g_fracture_source_id) continue;
        float t; int face;
        if (meshobject_ray_pick_face(objects[i], origin, dir, &t, &face)) {
            if (!best_obj || t < best_t) { best_obj = objects[i]; best_t = t; }
        }
    }

    float light_t;
    PhiLight *hit_light = light_ray_pick(origin, dir, &light_t);

    g_edit_face = -1;
    if (best_obj && (!hit_light || best_t <= light_t)) {
        ui_set_selected_object(4000u + (unsigned int)best_obj->id);
    } else if (hit_light) {
        ui_set_selected_object(LIGHT_ID_BASE + (unsigned int)hit_light->id);
    } else {
        ui_set_selected_object(0xFFFFFFFFu);
    }
}

/* ---- Main loop ---- */
static void main_loop(void *userdata) {
    (void)userdata;
    /* CPU-load capping (frame_pacer.h) -- brackets this frame's real work
     * (physics/animation/render, ending right after phi_platform_swap()
     * below), NOT the same thing as the dt clamp two lines down: that
     * clamp bounds physics/animation STEP SIZE after a slow frame, this
     * bounds actual CPU BUSY TIME every frame, always -- see frame_
     * pacer.h's own top comment for why this codebase had neither before. */
    frame_pacer_begin();
    double now = phi_platform_now();
    float dt = (float)(now - g_last_t);
    g_last_t = now;
    if (dt > 0.05f) dt = 0.05f;   /* cap at 50ms */

#ifndef __EMSCRIPTEN__
    net_poll_native();  /* wasm gets messages via an async JS callback instead */
#endif

    /* Gamepad/Steam Deck input (see input_gamepad.h) -- cheap, safe no-op
     * when nothing's connected; refreshes real button/axis state and
     * detects connect/disconnect once per frame. */
    phi_gamepad_poll();

    /* Phase 2 physics step -- see phi.md's "Bullet Physics via
     * Emscripten". phi_physics_world_step already subdivides into fixed
     * 1/60s substeps internally (see phi_physics.h), so passing this
     * frame's real (capped) dt straight through is correct, not just
     * convenient. Synced back to the MeshObject only if it actually has a
     * body -- most objects don't (phys_body is NULL by default, see
     * meshobject.h), so this is a no-op for the common case. */
    phi_physics_world_step(g_phys_world, dt);
    /* Phase 4's skinned test object -- advances its own playback and
     * recomputes world[]/skin[] every frame (pure CPU, see skinned_mesh_
     * object_update's own doc), independent of the physics step above
     * (this object has no physics body of its own outside of ragdoll
     * activation, which spawns SEPARATE capsule bodies -- see ragdoll.h --
     * rather than attaching one to this object directly). */
    if (g_skinned_test_loaded) skinned_mesh_object_update(&g_skinned_test_obj, dt);
    /* Every live skinned scene object (skinned_scene_objects.h) -- same
     * per-frame CPU pose->world->skin advance as the test object above,
     * just looped across the real registry instead of one fixed slot. */
    {
        SkinnedMeshObject *skinned_objs[SKINNED_SCENE_MAX_OBJECTS];
        int n_skinned = skinned_scene_object_get_all(skinned_objs);
        for (int i = 0; i < n_skinned; i++) skinned_mesh_object_update(skinned_objs[i], dt);
    }
    /* Syncs EVERY live scene object that has a physics body (most don't --
     * phys_body is NULL by default, see meshobject.h -- so this loop is a
     * no-op read for the common case), not just one fixed slot. */
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

    /* --- UI layout + click routing --- ui_layout() needs to run before
     * hit-testing (it's what computes every Area's on-screen rect), and
     * UIRenderContext is built once here and reused for the ui_render()
     * call further down too — one source of truth for what got clicked
     * vs. what actually gets drawn this frame, rather than two separately-
     * constructed contexts that could drift apart. */
    int cw, ch;
    phi_platform_get_window_size(&cw, &ch);
    ui_layout(cw, ch);
    /* Feeds this frame's cursor position into ui.c's border-hover
     * highlight (see ui_on_mouse_move) -- previously declared but never
     * actually called anywhere, since nothing needed live hover state
     * before area-border resize did. Layout must be current first (just
     * above) since hover hit-testing reads each Area's freshly-computed
     * x/y/w/h. */
    ui_on_mouse_move(g_inp.mouse_x, g_inp.mouse_y);

    /* Blender-style Object/Edit mode toggle -- reserved key, drained here
     * (same one-shot convention as every other *_edge field) before
     * UIRenderContext is built below so ui_ctx.editor_mode reflects any
     * toggle that happened this very frame, not last frame's mode. */
    if (g_inp.tab_edge) {
        g_inp.tab_edge = 0;
        toggle_editor_mode();
    }

    /* Blender-style modal G/S/R transform tool (client/transform_op.h) --
     * Grab/Scale/Rotate the selected MeshObject, available in BOTH Object
     * and Edit mode per explicit request (Edit mode still just has the
     * one object selected -- there's no per-vertex editing surface yet,
     * see g_edit_face's own comment, so "move the selected thing" means
     * the same object either way). Escape is drained unconditionally
     * here regardless of whether an op happens to be active this frame --
     * same "producer sets, consumer clears" requirement every other
     * one-shot InputState field in this codebase follows (see input.h),
     * even though right now the only thing that ever reads it is
     * transform_op_cancel below. */
    int xform_escape_pressed = g_inp.escape_edge;
    g_inp.escape_edge = 0;
    char xform_hud[64] = {0};
    {
        float sx, sy, sw, sh;
        int have_scene_rect = ui_get_scene_rect(&sx, &sy, &sw, &sh);

        if (!transform_op_active() && !gizmo_is_dragging() && !ui_is_modal_open() && have_scene_rect) {
            /* Entry: only starts when hovering the Scene panel's own
             * content (same hover-gated convention MMB-drag/scroll-wheel
             * routing already use elsewhere in this file), with a mesh
             * object OR a Light selected -- a Light only ever offers
             * Grab (see resolve_xform_target's own comment: no
             * orientation/scale field worth Scale/Rotate-ing), checked
             * per typed character below rather than gating the whole
             * block, so 's'/'r' with a light selected just falls through
             * to whatever else that keystroke would normally do (console/
             * chat text entry) instead of silently being swallowed here.
             * Only the FIRST matching character this frame is consumed
             * and removed from the queue -- any other character typed
             * the same frame still reaches the console/chat below
             * undisturbed. */
            int hovering_scene =
                (float)g_inp.mouse_x >= sx && (float)g_inp.mouse_x < sx + sw &&
                (float)g_inp.mouse_y >= sy && (float)g_inp.mouse_y < sy + sh;
            if (hovering_scene && (selected_mesh_object() || selected_light())) {
                for (int i = 0; i < g_inp.typed_count; i++) {
                    char c = g_inp.typed_chars[i];
                    TransformOpKind kind = XFORM_NONE;
                    if (c == 'g' || c == 'G') kind = XFORM_GRAB;
                    else if (c == 's' || c == 'S') kind = XFORM_SCALE;
                    else if (c == 'r' || c == 'R') kind = XFORM_ROTATE;
                    if (kind == XFORM_NONE) continue;

                    Vec3f *position; Quat *orientation; Vec3f *scale;
                    if (!resolve_xform_target(kind, &position, &orientation, &scale)) continue;   /* e.g. 's'/'r' with only a Light selected -- not a valid op, let the char fall through */

                    for (int j = i; j < g_inp.typed_count - 1; j++)
                        g_inp.typed_chars[j] = g_inp.typed_chars[j + 1];
                    g_inp.typed_count--;

                    Vec3f ro, rd;
                    compute_scene_ray(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y, &ro, &rd);
                    TransformCamCtx cam;
                    cam.ray_origin = ro; cam.ray_dir = rd;
                    cam_basis(g_cam_yaw, g_cam_pitch, &cam.fwd, &cam.right, &cam.up);
                    cam.distance = g_cam_distance;
                    transform_op_begin(kind, position, orientation, scale, &cam, g_inp.mouse_x, g_inp.mouse_y);
                    break;
                }
            }
        }

        if (transform_op_active() && have_scene_rect) {
            Vec3f ro, rd;
            compute_scene_ray(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y, &ro, &rd);
            TransformCamCtx cam;
            cam.ray_origin = ro; cam.ray_dir = rd;
            cam_basis(g_cam_yaw, g_cam_pitch, &cam.fwd, &cam.right, &cam.up);
            cam.distance = g_cam_distance;
            Vec3f *position; Quat *orientation; Vec3f *scale;
            resolve_xform_target(transform_op_kind(), &position, &orientation, &scale);
            transform_op_update(position, orientation, scale, &g_inp, &cam, g_inp.mouse_x, g_inp.mouse_y);

            /* Confirm: Enter or a plain LMB click. Cancel: Escape or a
             * plain RMB click. Draining lmb_click/rmb_click/enter_edge
             * here means the normal picking/context-menu blocks further
             * down this function naturally see them already cleared --
             * no separate transform_op_active() guard needed there. */
            if (g_inp.enter_edge) {
                g_inp.enter_edge = 0;
                transform_op_confirm();
            } else if (xform_escape_pressed) {
                transform_op_cancel(position, orientation, scale);
            } else if (g_inp.lmb_click) {
                g_inp.lmb_click = 0;
                transform_op_confirm();
            } else if (g_inp.rmb_click) {
                g_inp.rmb_click = 0;
                transform_op_cancel(position, orientation, scale);
            }
        } else if (transform_op_active()) {
            /* The Scene panel got swapped away mid-drag (the type-
             * switcher lets the user do this) -- no ray to update from;
             * still allow Escape/Enter to end the op cleanly rather than
             * leaving it stuck forever. */
            if (g_inp.enter_edge) {
                g_inp.enter_edge = 0;
                transform_op_confirm();
            } else if (xform_escape_pressed) {
                Vec3f *cancel_position; Quat *cancel_orientation; Vec3f *cancel_scale;
                resolve_xform_target(transform_op_kind(), &cancel_position, &cancel_orientation, &cancel_scale);
                transform_op_cancel(cancel_position, cancel_orientation, cancel_scale);
            }
        }

        transform_op_hud_text(xform_hud, (int)sizeof(xform_hud));
    }

    static const float light_dir[3] = {0.577f, 0.577f, 0.577f};
    float sky[3];
    renderer_get_sky_color(sky);
    UIRenderContext ui_ctx = {0};
    ui_ctx.renderer = g_renderer;
    ui_ctx.gbuf = g_gbuf;
    ui_ctx.test_obj = selected_mesh_object();   /* NULL if a Light or nothing is selected -- see UIRenderContext::test_obj's own updated comment */
    ui_ctx.edit_face = g_edit_face;
    ui_ctx.render_settings = &g_render_settings;
    ui_ctx.skinned_obj = g_skinned_test_loaded ? &g_skinned_test_obj : NULL;
    ui_ctx.editor_mode = g_editor_mode;
    ui_ctx.xform_hud = xform_hud;
    ui_ctx.console = &g_cs;
    ui_ctx.asset_browser = &g_ab;
    ui_ctx.chat = &g_chat;
    memcpy(ui_ctx.light_dir, light_dir, sizeof(light_dir));
    memcpy(ui_ctx.sky_color, sky, sizeof(sky));
    ui_ctx.draw_scene_content = scene_content_cb;
    ui_ctx.on_scene_zoom = scene_zoom_cb;

    /* Mouse wheel — rising-value-drained-here, same convention as every
     * other one-shot InputState field main.c reads. Routed by hover (see
     * ui_on_mouse_wheel) to whichever of Chat/Console/Asset Browser/
     * Python Panel's scrollback or the Scene panel's camera zoom
     * (scene_zoom_cb) the cursor is currently over. */
    if (g_inp.scroll_delta != 0) {
        ui_on_mouse_wheel(g_inp.mouse_x, g_inp.mouse_y, g_inp.scroll_delta, &ui_ctx);
        g_inp.scroll_delta = 0;
    }

    /* Blender-style Scene-panel camera navigation: MMB-drag orbits,
     * Ctrl+MMB-drag pans (see cam_orbit_from_start/cam_pan_from_start
     * above). Starts only when the press lands inside the Scene panel's
     * own rect -- same "must start inside the hit region, then continues
     * regardless of where the mouse goes" convention as ui.c's
     * area-border resize drag (ui_update_area_drag) -- so once a drag is
     * underway, the cursor leaving the Scene panel's bounds doesn't cut
     * it off mid-gesture. */
    if (g_inp.mmb_click) {
        g_inp.mmb_click = 0;   /* rising-edge flag -- MUST be drained here (see lmb_click/rmb_click just below), or this block re-fires and resets the drag's start position to the current mouse position EVERY frame, making every computed delta zero -- a real bug this exact wording caught: the drag could never produce any movement at all. */
        float sx, sy, sw, sh;
        if (ui_get_scene_rect(&sx, &sy, &sw, &sh) &&
            (float)g_inp.mouse_x >= sx && (float)g_inp.mouse_x < sx + sw &&
            (float)g_inp.mouse_y >= sy && (float)g_inp.mouse_y < sy + sh) {
            g_cam_dragging = 1;
            g_cam_drag_is_pan = g_inp.ctrl_down;
            g_cam_drag_start_x = g_inp.mouse_x;
            g_cam_drag_start_y = g_inp.mouse_y;
            g_cam_drag_start_yaw = g_cam_yaw;
            g_cam_drag_start_pitch = g_cam_pitch;
            g_cam_drag_start_pivot = g_cam_pivot;
        }
    }
    if (g_cam_dragging) {
        if (g_inp.mmb_down) {
            int dx = g_inp.mouse_x - g_cam_drag_start_x;
            int dy = g_inp.mouse_y - g_cam_drag_start_y;
            if (g_cam_drag_is_pan) cam_pan_from_start(dx, dy);
            else                    cam_orbit_from_start(dx, dy);
        } else {
            g_cam_dragging = 0;
        }
    }

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
                MeshObject *edit_sel = selected_mesh_object();
                if (g_editor_mode == EDITOR_MODE_EDIT && edit_sel) {
                    Vec3f origin, dir;
                    if (compute_scene_ray(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y, &origin, &dir)) {
                        float t; int face;
                        if (meshobject_ray_pick_face(edit_sel, origin, dir, &t, &face)) {
                            g_edit_face = face;
                            Vec3f world_hit = { origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t };
                            g_edit_hit_local = meshobject_world_to_local(edit_sel, world_hit);
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
            MeshObject *drag_sel = selected_mesh_object();
            if (drag_sel && ui_get_scene_rect(&sx, &sy, &sw, &sh)) {
                Vec3f origin, dir;
                if (compute_scene_ray(sx, sy, sw, sh, g_inp.mouse_x, g_inp.mouse_y, &origin, &dir)) {
                    gizmo_update_drag(&drag_sel->position, origin, dir);
                }
            }
        } else {
            gizmo_end_drag();
        }
    }

    /* Area border drag-to-resize -- same continuous per-frame shape as
     * the gizmo drag block just above, driving ui.c's own resize state
     * instead of the gizmo's. A no-op whenever nothing's being resized. */
    ui_update_area_drag(g_inp.mouse_x, g_inp.mouse_y, g_inp.lmb_down);

    /* Scene context-menu action, drained once per frame like the click
     * flags above (captured into a local since it's a one-shot poll --
     * the switch itself needs the specific action more than once when
     * it's one of the four Add Light rows, which all fall into the same
     * case block). Only Add Mesh Object / Add Light / Delete / Extrude /
     * Inset / Loop Cut / Fracture are wired to real behavior — Frame
     * Selected/Frame All/Deselect All are still placeholder rows (see
     * ui_poll_context_menu_action's own comment). */
    CtxMenuAction ctx_action = ui_poll_context_menu_action();
    switch (ctx_action) {
        case CTX_ACTION_ADD_MESH:
            /* Real multi-object support -- always adds a NEW cube (no
             * "already exists" refusal anymore), at the camera's own
             * orbit pivot, same spawn-point convention Add Light already
             * established. Deliberately allowed to clip through whatever
             * else is already there, separable afterward with G. */
            add_mesh_object_from_path("assets/cube.gltf", 16.0f, g_cam_pivot);
            break;
        case CTX_ACTION_ADD_LIGHT_POINT:
        case CTX_ACTION_ADD_LIGHT_SUN:
        case CTX_ACTION_ADD_LIGHT_SPOT:
        case CTX_ACTION_ADD_LIGHT_AREA: {
            /* Spawns at the camera's own orbit pivot -- roughly "the
             * point the user is currently looking at", the closest thing
             * this engine has to Blender's 3D cursor (which doesn't
             * exist here) as a sensible default spawn point. */
            LightType lt = (ctx_action == CTX_ACTION_ADD_LIGHT_POINT) ? LIGHT_TYPE_POINT
                          : (ctx_action == CTX_ACTION_ADD_LIGHT_SUN)   ? LIGHT_TYPE_SUN
                          : (ctx_action == CTX_ACTION_ADD_LIGHT_SPOT)  ? LIGHT_TYPE_SPOT
                          :                                              LIGHT_TYPE_AREA;
            PhiLight *l = light_spawn(lt, g_cam_pivot);
            if (l) {
                ui_set_selected_object(LIGHT_ID_BASE + (unsigned int)l->id);
                printf("[main] context menu Add Light: spawned #%d at pivot (%.1f, %.1f, %.1f)\n",
                       l->id, g_cam_pivot.x, g_cam_pivot.y, g_cam_pivot.z);
            } else {
                printf("[main] context menu Add Light: light registry is full (PHI_MAX_LIGHTS)\n");
            }
            break;
        }
        case CTX_ACTION_DELETE: {
            MeshObject *sel_obj = selected_mesh_object();
            PhiLight *sel_light = selected_light();
            if (sel_obj) {
                delete_mesh_object(sel_obj);
            } else if (sel_light) {
                int id = sel_light->id;
                light_delete(id);
                ui_set_selected_object(0xFFFFFFFFu);
                printf("[main] context menu Delete: removed Light #%d\n", id);
            } else {
                printf("[main] context menu Delete: nothing selected\n");
            }
            break;
        }
        case CTX_ACTION_EXTRUDE_FACE: {
            MeshObject *sel = selected_mesh_object();
            if (sel && g_edit_face >= 0) {
                int before = sel->render_mesh->count / 3;
                /* Fixed 4-unit offset along the face's own normal -- no
                 * mouse-driven interactive extrude distance in this pass,
                 * matching this loop's "minimal, correct, not Blender-grade
                 * polish" bar (see mesh_edit.h). */
                int cap = mesh_edit_extrude_face(sel->hem, g_edit_face, 4.0f);
                if (cap >= 0) {
                    rebuild_mesh_render(sel, "Extrude Face", before);
                    g_edit_face = cap;  /* new cap becomes the natural next target, e.g. a chained extrude */
                } else {
                    printf("[main] context menu Extrude Face: operation failed (non-triangle or stale face)\n");
                }
            } else {
                printf("[main] context menu Extrude Face: no face under the last right-click\n");
            }
            break;
        }
        case CTX_ACTION_INSET_FACE: {
            MeshObject *sel = selected_mesh_object();
            if (sel && g_edit_face >= 0) {
                int before = sel->render_mesh->count / 3;
                int cap = mesh_edit_inset_face(sel->hem, g_edit_face, 0.4f);
                if (cap >= 0) {
                    rebuild_mesh_render(sel, "Inset Face", before);
                    g_edit_face = cap;
                } else {
                    printf("[main] context menu Inset Face: operation failed (non-triangle or stale face)\n");
                }
            } else {
                printf("[main] context menu Inset Face: no face under the last right-click\n");
            }
            break;
        }
        case CTX_ACTION_LOOP_CUT: {
            MeshObject *sel = selected_mesh_object();
            if (sel && g_edit_face >= 0) {
                int before = sel->render_mesh->count / 3;
                int edge = mesh_edit_nearest_edge_of_face(sel->hem, g_edit_face,
                                                            g_edit_hit_local.x, g_edit_hit_local.y, g_edit_hit_local.z);
                int mv = edge >= 0 ? mesh_edit_loop_cut_edge(sel->hem, edge) : -1;
                if (mv >= 0) {
                    rebuild_mesh_render(sel, "Loop Cut", before);
                } else {
                    printf("[main] context menu Loop Cut: operation failed (no edge/stale face)\n");
                }
                g_edit_face = -1;  /* the picked face was deleted by the cut, don't keep pointing at it */
            } else {
                printf("[main] context menu Loop Cut: no face under the last right-click\n");
            }
            break;
        }
        case CTX_ACTION_FRACTURE:
            /* Computes Voronoi fragments (fracture.h) and now does BOTH
             * things that capability was always meant to support: saves
             * them to disk (fracture_save_glb, real asset-pipeline value
             * independent of runtime activation) AND activates them live
             * in the running physics world (fracture_body_activate --
             * Phase 2's "shatter on impact" completion, see fracture_
             * body.h) -- the SAME seed drives both, so they're the
             * identical fragmentation rather than two independent random
             * splits. Fixed 8-fragment count and a real time-based seed,
             * no interactive fragment-count/threshold picker UI, matching
             * this codebase's established "real but not Blender-grade
             * polish" bar for editor actions (see e.g. Extrude's own
             * fixed 4-unit offset). breaking_threshold is a reasoned
             * starting point (well above the ~50-unit threshold
             * phi_physics_test verified breaks/holds correctly at, since
             * this object is larger/heavier and may fall before
             * shattering) but genuinely UNVERIFIED live -- this sandbox
             * can't open a real GL window to watch it actually shatter
             * and tune the feel, flagged honestly rather than claimed
             * correct by construction. */
            {
                MeshObject *sel = selected_mesh_object();
                if (sel) {
                    const int n_frag = 8;
                    const float breaking_threshold = 200.0f;
                    unsigned int seed = (unsigned int)phi_platform_now();

                    FractureFragment *frags = fracture_voronoi(sel, n_frag, seed);
                    if (frags) {
                        int ok = fracture_save_glb(frags, n_frag, "assets/fracture_output.gltf");
                        int non_empty = 0;
                        for (int i = 0; i < n_frag; i++) if (frags[i].pos_count > 0) non_empty++;
                        printf("[main] context menu Fracture: %d/%d non-empty fragments, %s -> assets/fracture_output.gltf\n",
                               non_empty, n_frag, ok ? "saved" : "SAVE FAILED");
                        fracture_free_fragments(frags, n_frag);
                    } else {
                        printf("[main] context menu Fracture: precompute failed (no hem?)\n");
                    }

                    int spawned = fracture_body_activate(g_phys_world, sel->hem,
                                                          sel->position, sel->orientation,
                                                          n_frag, seed, breaking_threshold);
                    if (spawned > 0) {
                        g_fracture_active = 1;
                        g_fracture_source_id = sel->id;
                        ui_set_selected_object(0xFFFFFFFFu);
                        g_edit_face = -1;
                        printf("[main] context menu Fracture: activated %d live fragment bodies\n", spawned);
                    } else {
                        printf("[main] context menu Fracture: activation produced no live fragments\n");
                    }
                } else {
                    printf("[main] context menu Fracture: no MeshObject selected\n");
                }
            }
            break;
        case CTX_ACTION_SAVE_AS_ASSET:
            /* Starts a pending create in the Asset Browser's shared edit
             * form (see asset_browser_begin_create) -- doesn't upload
             * anything yet, that only happens once g_ab.create_requested
             * fires from actually submitting the form (see below). Which
             * object gets uploaded at that later point is remembered by
             * id (g_save_as_asset_object_id), not re-resolved from
             * "whichever is selected" -- selection could change in
             * between opening the form and submitting it. */
            {
                MeshObject *sel = selected_mesh_object();
                if (sel) {
                    g_save_as_asset_object_id = sel->id;
                    asset_browser_begin_create(&g_ab, "MeshObject");
                } else {
                    printf("[main] context menu Save as Asset: no MeshObject selected\n");
                }
            }
            break;
        case CTX_ACTION_ENABLE_PHYSICS:
            /* Convex hull shape from the mesh's own real vertex positions
             * (real Bullet quickhull reduction, see
             * phi_physics_add_convex_hull_body's own comment) -- Blender's
             * own default collision shape for dynamic ("Active") rigid
             * bodies too, confirmed by reading its rigidbody.cc directly
             * rather than assumed. Box was this project's original
             * placeholder, chosen only because convex hulls weren't
             * implemented yet (see phi.md's Phase 2 status) -- the static
             * ground body at startup stays box, which is both correct for
             * a flat plane and matches Blender's own box/trimesh-for-
             * passive-objects default. mass=1.0 (dynamic), a modest
             * restitution so it doesn't bounce forever. Once created,
             * main_loop's frame step syncs position/orientation FROM the
             * simulated body every frame instead of leaving them alone. */
            {
                MeshObject *sel = selected_mesh_object();
                if (!sel) {
                    printf("[main] context menu Enable Physics: no MeshObject selected\n");
                } else if (sel->phys_body) {
                    printf("[main] context menu Enable Physics: already has a physics body\n");
                } else if (!sel->hem || sel->hem->vert_count < 4) {
                    printf("[main] context menu Enable Physics: not enough vertices for a hull (need a real 3D mesh)\n");
                } else {
                    HalfEdgeMesh *hem = sel->hem;
                    float *flat = (float *)malloc((size_t)hem->vert_count * 3 * sizeof(float));
                    for (int i = 0; i < hem->vert_count; i++) {
                        flat[i*3+0] = hem->verts[i].pos[0];
                        flat[i*3+1] = hem->verts[i].pos[1];
                        flat[i*3+2] = hem->verts[i].pos[2];
                    }
                    float orientation[4] = {
                        sel->orientation.x, sel->orientation.y,
                        sel->orientation.z, sel->orientation.w
                    };
                    sel->phys_body = phi_physics_add_convex_hull_body(
                        g_phys_world, flat, hem->vert_count, sel->position, orientation, 1.0f, 0.3f);
                    printf("[main] context menu Enable Physics: convex hull from %d vertices, mass=1.0\n",
                           hem->vert_count);
                    free(flat);
                }
            }
            break;
        case CTX_ACTION_TOGGLE_EDIT_MODE:
            toggle_editor_mode();
            break;
        default:
            break;
    }

    /* Top menu bar action (File > Save/Load, see ui.h's TopMenuAction) --
     * drained once per frame like the context-menu action just above.
     * Both rows reuse an EXISTING mechanism rather than duplicating it:
     * Save is the identical "begin the Asset Browser's create flow"
     * CTX_ACTION_SAVE_AS_ASSET already does; Load sets the exact same
     * AssetBrowserState::load_requested/load_requested_id fields the
     * Asset Browser panel's own per-row Load button sets (see ui.c's
     * hit_test_area), for whichever asset is currently selected there --
     * the block further down that actually performs a load (g_ab.
     * load_requested) doesn't know or care which UI triggered it. */
    switch (ui_poll_top_menu_action()) {
        case TOP_ACTION_FILE_SAVE: {
            MeshObject *sel = selected_mesh_object();
            if (sel) {
                g_save_as_asset_object_id = sel->id;   /* see its own comment -- remembered by id, re-resolved at actual-upload time */
                asset_browser_begin_create(&g_ab, "MeshObject");
            } else {
                printf("[main] File > Save: no MeshObject selected\n");
            }
            break;
        }
        case TOP_ACTION_FILE_LOAD:
            if (g_ab.selected >= 0 && g_ab.selected < g_ab.count) {
                g_ab.load_requested = 1;
                g_ab.load_requested_id = g_ab.items[g_ab.selected].id;
            } else {
                printf("[main] File > Load: no asset selected in the Asset Browser panel\n");
            }
            break;
        case TOP_ACTION_FILE_RENDER:
            render_still_frame_to_disk(NULL, 0);
            break;
        default:
            break;
    }

    /* Timeline panel one-shot intent (Play/Pause, scrub) -- drained here,
     * same "poll once per frame, mutate the real state main.c owns"
     * pattern the top-menu/context-menu actions just above already use.
     * ui.c never touches g_skinned_test_obj.playback directly (see
     * UIRenderContext::skinned_obj's own comment). */
    if (g_skinned_test_loaded && g_skinned_test_obj.playback.clip) {
        if (ui_poll_timeline_play_toggle()) {
            g_skinned_test_obj.playback.playing = !g_skinned_test_obj.playback.playing;
        }
        float frac;
        if (ui_poll_timeline_scrub(&frac)) {
            g_skinned_test_obj.playback.time = frac * g_skinned_test_obj.playback.clip->duration;
        }
    }

    /* Exactly one text field gets this frame's keystrokes: whichever of
     * the Asset Browser's search/edit_name/edit_tags fields is focused
     * (a click inside it, see ui.c's hit_test_area), else the Chat input
     * if IT is focused (a click inside it, same convention), else the
     * Python console -- which is back to being the unconditional default
     * the moment nothing else is focused, same as before this panel's
     * text fields existed. Both asset_browser_update_focused_text and
     * chat_update_focused_text are no-ops that leave InputState untouched
     * when their own focus is NONE, so falling all the way through to
     * pyconsole_update is always correct, not just "usually". Skipped
     * entirely while a modal G/S/R transform op is active -- it already
     * drains typed_chars/backspace_edge itself (see transform_op_update),
     * so this would see an empty queue anyway, but skipping outright
     * keeps "a modal op owns all keyboard input while active" true
     * without relying on that as an implementation detail. */
    if (!transform_op_active()) {
        if (g_ab.focus != AB_FOCUS_NONE) {
            asset_browser_update_focused_text(&g_ab, &g_inp);
        } else if (g_chat.focus != CHAT_FOCUS_NONE) {
            chat_update_focused_text(&g_chat, &g_inp);
        } else if (ui_is_editing_prop()) {
            ui_update_prop_edit_text(&g_inp);
        } else {
            pyconsole_update(&g_cs, &g_inp);
        }
    }

    /* Chat one-shot send, same "UI raises intent, main.c executes"
     * pattern as the Asset Browser flags right below -- echoes the
     * outgoing message locally (a real server round trip is not
     * instant), then hands it to net.c. waiting_for_reply (the "Claude is
     * thinking..." indicator) is only set when the message actually
     * contains "@llm" -- server.py's own PKT_CHAT_MSG handler silently
     * does NOT invoke the model (and so never sends a PKT_CHAT_REPLY back)
     * for a message without it, matching a Slack-bot-style @mention
     * convention; without this check the indicator would spin forever on
     * a reply that was never coming for any message that doesn't mention
     * the model. */
    if (g_chat.send_requested) {
        g_chat.send_requested = 0;
        char echo[CHAT_LINE_LEN];
        snprintf(echo, sizeof(echo), "You: %s", g_chat.pending_send);
        chat_append_multiline(echo);
        if (strstr(g_chat.pending_send, "@llm") != NULL) g_chat.waiting_for_reply = 1;
        net_send_chat_msg(&g_ns, g_chat.pending_send);
    }

    /* Asset Browser one-shot request flags, drained once per frame -- same
     * "UI raises intent, main.c executes it against the engine/network"
     * shape as the context-menu action poll above. Set by ui.c's click
     * routing (hit_test_area's PANEL_ASSET_BROWSER block), cleared here
     * whether or not the underlying action actually succeeds (a failed
     * load/connect isn't a reason to keep retrying every frame). */
    if (g_ab.refresh_requested) {
        g_ab.refresh_requested = 0;
        /* Whatever's currently typed in the search bar filters the
         * request (server-side LIKE against name/tag, see assets_db.py) --
         * empty search means no filter, same as passing NULL. Applies
         * equally whether this fired from clicking Refresh or pressing
         * Enter in the search bar, so Refresh always means "re-run
         * whatever's currently searched for". */
        net_send_asset_list_request(&g_ns, g_ab.search[0] ? g_ab.search : NULL);
    }
    if (g_ab.load_requested) {
        g_ab.load_requested = 0;
        const char *path = NULL;
        for (int i = 0; i < g_ab.count; i++) {
            if (g_ab.items[i].id == g_ab.load_requested_id) { path = g_ab.items[i].path; break; }
        }
        if (path) add_mesh_object_from_path(path, 1.0f, g_cam_pivot);   /* adds a NEW object, allowed to clip through whatever's already there -- see add_mesh_object_from_path's own comment */
        else printf("[main] asset browser: load requested for id=%u, not in the current list\n", g_ab.load_requested_id);
    }
    if (g_ab.delete_requested) {
        g_ab.delete_requested = 0;
        net_send_asset_delete(&g_ns, g_ab.delete_requested_id);
    }
    if (g_ab.update_requested) {
        g_ab.update_requested = 0;
        /* editing_id is always >= 0 here -- asset_browser_update_focused_
         * text only ever sets update_requested from the AB_FOCUS_EDIT_TAGS
         * case, and only when editing_id >= 0 (see its own switch). */
        net_send_asset_update(&g_ns, (uint32_t)g_ab.editing_id, g_ab.edit_name, g_ab.edit_tags);
        asset_browser_cancel_edit(&g_ab);
    }
    if (g_ab.create_requested) {
        g_ab.create_requested = 0;
#ifdef PHI_HAVE_HTTP_CLIENT
        MeshObject *save_obj = scene_object_find(g_save_as_asset_object_id);
        if (!save_obj || !save_obj->hem) {
            printf("[main] asset browser: create requested, but the object being saved no longer exists\n");
        } else {
            uint8_t *glb; int glb_len;
            if (!halfedge_save_glb_buffer(save_obj->hem, &glb, &glb_len)) {
                printf("[main] asset browser: failed to flatten the current MeshObject to GLB\n");
            } else {
                char name_enc[192], tags_enc[192], path_and_query[512];
                http_url_encode(g_ab.edit_name[0] ? g_ab.edit_name : "Untitled", name_enc, sizeof(name_enc));
                http_url_encode(g_ab.edit_tags, tags_enc, sizeof(tags_enc));
                snprintf(path_and_query, sizeof(path_and_query), "/assets?name=%s&tags=%s", name_enc, tags_enc);

                char host[128]; int port;
                if (http_parse_ws_host_port(g_ns.ws_url, host, sizeof(host), &port) != 0) {
                    printf("[main] asset browser: couldn't parse a host:port to POST to from '%s'\n", g_ns.ws_url);
                } else {
                    char resp[256]; int status;
                    int ok = http_post_native(host, port, path_and_query, glb, glb_len, resp, sizeof(resp), &status);
                    printf("[main] asset browser: POST %s -> ok=%d status=%d resp=\"%s\"\n",
                           path_and_query, ok, status, resp);
                    if (ok && status == 200) {
                        asset_browser_cancel_edit(&g_ab);
                        g_ab.refresh_requested = 1;   /* picked up next frame -- shows the new asset immediately */
                    }
                }
                free(glb);
            }
        }
#else
        printf("[main] asset browser: create isn't available on this build target "
               "(no HTTP client -- see http_client_native.h's comment)\n");
        asset_browser_cancel_edit(&g_ab);
#endif
    }

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

    /* One-shot: confirm the Phase 1 startup test MeshObject (the first
     * object in the scene registry, object_id 4001 at startup -- see
     * spawn_test_mesh_object) is genuinely rasterized SOMEWHERE on
     * screen, not just loaded into memory — a coarse full-framebuffer
     * object-id scan rather than relying on the center pixel happening
     * to land on it. G-buffer-local coordinates throughout (see the
     * comment above). */
    static int s_mesh_obj_checked = 0;
    MeshObject *startup_check_objects[SCENE_MAX_OBJECTS];
    int startup_check_count = (have_scene && !s_mesh_obj_checked && s_frame == 30) ? scene_object_get_all(startup_check_objects) : 0;
    if (startup_check_count > 0) {
        s_mesh_obj_checked = 1;
        unsigned int watch_id = 4000u + (unsigned int)startup_check_objects[0]->id;
        int found = 0, min_x = -1, max_x = -1, min_y = -1, max_y = -1;
        for (int y = 0; y < g_gbuf->h; y += 4) {
            for (int x = 0; x < g_gbuf->w; x += 4) {
                unsigned int id = gbuffer_pick_object_id(g_gbuf, x, y);
                if (id == watch_id) {
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
    frame_pacer_end();
}

/* Answers PKT_SCENE_STATE_REQUEST (see net.h's own comment on why this
 * lives in main.c and not net.c: the live MeshObject/physics-world state
 * being asked about is main.c's global state, per this project's client-
 * authored architecture) -- the get_scene_state tool server/
 * anthropic_client.py's chat loop can call. Hand-built JSON (no JSON
 * library anywhere in this C codebase) rather than a bespoke text format,
 * since every value here is either a bool/number or a small fixed nested
 * object -- no free text, so no escaping to get right. */
/* Chat-driven scene mutation handlers (see net.h's PKT_PROP_SET_REQUEST
 * comment) -- registered with net.c below, same "net.c parses the wire
 * payload, main.c owns and mutates the actual live state" division of
 * labor scene_state_handler already established for the read-only
 * counterpart. Claude's access here is deliberately read-WRITE per this
 * project's own explicit decision (unlike get_asset_list/get_scene_state,
 * which stay read-only) -- these three are the entire write surface: set
 * an existing light/render-settings prop, spawn a light, delete a light.
 * No object/face mutation exposed to chat (scene_resolve_target's
 * "object"/"face" targets still work here mechanically, but
 * server/anthropic_client.py's own tool list is what actually gates what
 * Claude can invoke, and it only defines light/render tools -- see
 * phi.md's Phase 3 status). */
static void prop_set_handler(uint32_t req_id, const char *target, const char *identifier,
                              int is_vec3, float v0, float v1, float v2) {
    const PhiPropGroup *group; void *owner;
    int ok = 0;
    if (scene_resolve_target(target, &group, &owner)) {
        const PhiProp *prop = phi_prop_find(group, identifier);
        if (prop) {
            if (is_vec3 && prop->type == PHI_PROP_VEC3) {
                float v[3] = {v0, v1, v2};
                ok = phi_prop_set_vec3(owner, prop, v);
            } else if (!is_vec3 && prop->type != PHI_PROP_VEC3) {
                ok = phi_prop_set_float(owner, prop, v0);
            }
        }
    }
    net_send_prop_set_reply(&g_ns, req_id, ok);
}

static void add_light_handler(uint32_t req_id, int type, float x, float y, float z) {
    LightType lt = (type >= 0 && type < LIGHT_TYPE_COUNT) ? (LightType)type : LIGHT_TYPE_POINT;
    PhiLight *l = light_spawn(lt, (Vec3f){x, y, z});
    net_send_add_light_reply(&g_ns, req_id, l != NULL, l ? (uint32_t)l->id : 0u);
}

static void delete_light_handler(uint32_t req_id, uint32_t light_id) {
    int ok = light_delete((int)light_id);
    net_send_delete_light_reply(&g_ns, req_id, ok);
}

/* Chat-driven geometry creation/editing (Phase 5, see net.h's own
 * comment on PKT_CREATE_MESH_REQUEST/PKT_SET_VERTICES_REQUEST/PKT_
 * DELETE_MESH_OBJECT_REQUEST) -- same "server asks THIS client to act"
 * shape as add_light_handler/delete_light_handler above, calling the
 * exact same scene_objects.c/halfedge.c machinery main.c's own context-
 * menu actions and mp_port.c's phi.create_mesh/set_vertices already use
 * -- Claude's chat access to this is the actual point of this section
 * ("make it so Claude can create geometry and redefine the verts of
 * existing scene geometry"), not a separate implementation of it. */
static void create_mesh_handler(uint32_t req_id, float x, float y, float z,
                                 const float *positions, int vert_count,
                                 const unsigned short *indices, int index_count) {
    HalfEdgeMesh *hem = halfedge_build_from_triangles(positions, vert_count, indices, index_count);
    if (!hem) {
        net_send_create_mesh_reply(&g_ns, req_id, 0, 0);
        return;
    }
    MeshObject *obj = scene_object_add();
    if (!obj) {
        halfedge_destroy(hem);
        net_send_create_mesh_reply(&g_ns, req_id, 0, 0);
        return;
    }
    obj->position = (Vec3f){x, y, z};
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj->is_static = 1;
    obj->render_mesh = mesh_create();
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    obj->hem = hem;
    printf("[main] chat create_mesh_object: created MeshObject #%d (%d verts, %d indices)\n", obj->id, vert_count, index_count);
    net_send_create_mesh_reply(&g_ns, req_id, 1, (uint32_t)obj->id);
}

static void set_vertices_handler(uint32_t req_id, uint32_t object_id, const float *positions, int vert_count) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj || !obj->hem || obj->hem->vert_count != vert_count) {
        net_send_set_vertices_reply(&g_ns, req_id, 0);
        return;
    }
    for (int i = 0; i < vert_count; i++) {
        obj->hem->verts[i].pos[0] = positions[i*3+0];
        obj->hem->verts[i].pos[1] = positions[i*3+1];
        obj->hem->verts[i].pos[2] = positions[i*3+2];
    }
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    printf("[main] chat set_mesh_vertices: updated %d vertices on MeshObject #%u\n", vert_count, object_id);
    net_send_set_vertices_reply(&g_ns, req_id, 1);
}

static void delete_mesh_object_handler(uint32_t req_id, uint32_t object_id) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj) {
        net_send_delete_mesh_object_reply(&g_ns, req_id, 0);
        return;
    }
    delete_mesh_object(obj);
    net_send_delete_mesh_object_reply(&g_ns, req_id, 1);
}

/* Incremental mesh editing for chat (see net.h's PKT_GET_MESH_VERTICES_
 * REQUEST comment) -- "the model can see where vertices are, add
 * vertices to an existing mesh, move individual vertices". Same
 * halfedge_add_vertex/halfedge_add_face/direct hem->verts access
 * mp_port.c's phi.get_vertices/get_faces/add_vertex/add_face/set_vertex
 * already use for the LOCAL Python console -- this is the identical
 * capability, reached over the wire instead. */
static void get_mesh_vertices_handler(uint32_t req_id, uint32_t object_id) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj || !obj->hem) {
        net_send_get_mesh_vertices_reply(&g_ns, req_id, 0, NULL, 0);
        return;
    }
    HalfEdgeMesh *hem = obj->hem;
    int vc = hem->vert_count;
    if (vc > PKT_MESH_MAX_VERTS) vc = PKT_MESH_MAX_VERTS;   /* wire cap -- see net.h */
    static float pos_buf[PKT_MESH_MAX_VERTS * 3];
    for (int i = 0; i < vc; i++) {
        pos_buf[i*3+0] = hem->verts[i].pos[0];
        pos_buf[i*3+1] = hem->verts[i].pos[1];
        pos_buf[i*3+2] = hem->verts[i].pos[2];
    }
    net_send_get_mesh_vertices_reply(&g_ns, req_id, 1, pos_buf, vc);
}

static void get_mesh_faces_handler(uint32_t req_id, uint32_t object_id) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj || !obj->hem) {
        net_send_get_mesh_faces_reply(&g_ns, req_id, 0, NULL, NULL, 0);
        return;
    }
    HalfEdgeMesh *hem = obj->hem;
    static uint16_t face_idx_buf[PKT_MESH_MAX_TRIS];
    static uint16_t vert_buf[PKT_MESH_MAX_TRIS * 3];
    int n = 0;
    for (int f = 0; f < hem->face_count && n < PKT_MESH_MAX_TRIS; f++) {
        if (hem->faces[f].deleted) continue;
        if (hem->faces[f].count != 3) continue;   /* wire format is triangles-only, see net.h */
        int verts[3];
        halfedge_face_verts(hem, f, verts);
        face_idx_buf[n] = (uint16_t)f;
        vert_buf[n*3+0] = (uint16_t)verts[0];
        vert_buf[n*3+1] = (uint16_t)verts[1];
        vert_buf[n*3+2] = (uint16_t)verts[2];
        n++;
    }
    net_send_get_mesh_faces_reply(&g_ns, req_id, 1, face_idx_buf, vert_buf, n);
}

/* Adding a lone vertex doesn't touch the render mesh -- same reasoning as
 * mp_port.c's native_add_vertex (meshobject_build_render_mesh_from_
 * halfedge only ever walks live FACES). No rebuild call here for that
 * reason; add_mesh_face_handler below is what actually makes a just-
 * added vertex visible. */
static void add_mesh_vertex_handler(uint32_t req_id, uint32_t object_id, float x, float y, float z) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj || !obj->hem) {
        net_send_add_mesh_vertex_reply(&g_ns, req_id, 0, 0);
        return;
    }
    int idx = halfedge_add_vertex(obj->hem, x, y, z);
    printf("[main] chat add_mesh_vertex: added vertex %d to MeshObject #%u\n", idx, object_id);
    net_send_add_mesh_vertex_reply(&g_ns, req_id, 1, (uint32_t)idx);
}

static void add_mesh_face_handler(uint32_t req_id, uint32_t object_id, uint16_t v0, uint16_t v1, uint16_t v2) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj || !obj->hem) {
        net_send_add_mesh_face_reply(&g_ns, req_id, 0, 0);
        return;
    }
    /* Bounds-checked here because halfedge_add_face itself does not -- an
     * out-of-range index writes straight past hem->verts (same reasoning
     * as mp_port.c's native_add_face). */
    if (v0 >= obj->hem->vert_count || v1 >= obj->hem->vert_count || v2 >= obj->hem->vert_count) {
        net_send_add_mesh_face_reply(&g_ns, req_id, 0, 0);
        return;
    }
    int verts[3] = { v0, v1, v2 };
    int f = halfedge_add_face(obj->hem, verts, 3);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    printf("[main] chat add_mesh_face: added face %d (%u,%u,%u) to MeshObject #%u\n", f, v0, v1, v2, object_id);
    net_send_add_mesh_face_reply(&g_ns, req_id, 1, (uint32_t)f);
}

static void set_mesh_vertex_handler(uint32_t req_id, uint32_t object_id, uint16_t vertex_index, float x, float y, float z) {
    MeshObject *obj = scene_object_find((int)object_id);
    if (!obj || !obj->hem || vertex_index >= obj->hem->vert_count) {
        net_send_set_mesh_vertex_reply(&g_ns, req_id, 0);
        return;
    }
    obj->hem->verts[vertex_index].pos[0] = x;
    obj->hem->verts[vertex_index].pos[1] = y;
    obj->hem->verts[vertex_index].pos[2] = z;
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    printf("[main] chat set_mesh_vertex: moved vertex %u on MeshObject #%u\n", vertex_index, object_id);
    net_send_set_mesh_vertex_reply(&g_ns, req_id, 1);
}

static void scene_state_handler(uint32_t req_id) {
    /* Every live scene object (Phase 5's real multi-object support, see
     * phi.md) -- id/position/orientation/is_static/vert_count/
     * face_count/has_physics/velocity per object, replacing the old
     * single flat set of fields this used to report for the one fixed
     * slot. Claude's get_scene_state tool sees the whole scene now, not
     * just one object -- a real prerequisite for create_mesh_object/
     * set_vertices (see net.h) to be usable at all: you can't sensibly
     * ask Claude to edit "the object" when there could be several. */
    char objects_json[6144];
    {
        MeshObject *objects[SCENE_MAX_OBJECTS];
        int n = scene_object_get_all(objects);
        char *op = objects_json;
        size_t remaining = sizeof(objects_json);
        int written = snprintf(op, remaining, "[");
        op += written; remaining -= (size_t)written;
        for (int i = 0; i < n && remaining > 200; i++) {
            MeshObject *o = objects[i];
            int vc = o->hem ? o->hem->vert_count : 0;
            int fc = o->hem ? o->hem->face_count : 0;
            int has_phys = o->phys_body != NULL;
            Vec3f v = has_phys ? phi_physics_get_linear_velocity(o->phys_body) : (Vec3f){0.0f, 0.0f, 0.0f};
            written = snprintf(op, remaining,
                "%s{\"id\":%d,\"position\":[%.3f,%.3f,%.3f],\"orientation\":[%.3f,%.3f,%.3f,%.3f],"
                "\"is_static\":%s,\"vert_count\":%d,\"face_count\":%d,\"has_physics\":%s,"
                "\"velocity\":[%.3f,%.3f,%.3f]}",
                i > 0 ? "," : "", o->id, o->position.x, o->position.y, o->position.z,
                o->orientation.x, o->orientation.y, o->orientation.z, o->orientation.w,
                o->is_static ? "true" : "false", vc, fc, has_phys ? "true" : "false",
                v.x, v.y, v.z);
            op += written; remaining -= (size_t)written;
        }
        snprintf(op, remaining, "]");
    }

    MeshObject *sel = selected_mesh_object();
    int has_edit_face = (g_edit_face >= 0 && sel && sel->hem && g_edit_face < sel->hem->face_count);
    char face_json[256];
    if (has_edit_face) {
        HEFace *ef = &sel->hem->faces[g_edit_face];
        snprintf(face_json, sizeof(face_json),
                 "{\"object_id\":%d,\"index\":%d,\"base_color\":[%.3f,%.3f,%.3f],\"metallic\":%.3f,"
                 "\"roughness\":%.3f,\"emission\":[%.3f,%.3f,%.3f]}",
                 sel->id, g_edit_face, ef->base_color[0], ef->base_color[1], ef->base_color[2],
                 ef->metallic, ef->roughness, ef->emission[0], ef->emission[1], ef->emission[2]);
    } else {
        snprintf(face_json, sizeof(face_json), "null");
    }

    /* Lights + render settings (Phase 3, see light.h/render_settings.h)
     * -- read-only here (this is get_scene_state); the write side is
     * prop_set_handler/add_light_handler/delete_light_handler above. */
    char lights_json[3072];
    {
        static const char *type_names[LIGHT_TYPE_COUNT] = {"point", "sun", "spot", "area"};
        PhiLight *lights[PHI_MAX_LIGHTS];
        int n = light_get_all(lights);
        char *lp = lights_json;
        size_t remaining = sizeof(lights_json);
        int written = snprintf(lp, remaining, "[");
        lp += written; remaining -= (size_t)written;
        for (int i = 0; i < n && remaining > 128; i++) {
            written = snprintf(lp, remaining,
                "%s{\"id\":%d,\"type\":\"%s\",\"position\":[%.3f,%.3f,%.3f],\"direction\":[%.3f,%.3f,%.3f],"
                "\"color\":[%.3f,%.3f,%.3f],\"energy\":%.3f,\"radius\":%.3f,\"spot_size\":%.4f,"
                "\"spot_blend\":%.3f,\"area_size\":%.3f,\"sun_angle\":%.5f}",
                i > 0 ? "," : "", lights[i]->id, type_names[lights[i]->type],
                lights[i]->position.x, lights[i]->position.y, lights[i]->position.z,
                lights[i]->direction.x, lights[i]->direction.y, lights[i]->direction.z,
                lights[i]->color.x, lights[i]->color.y, lights[i]->color.z,
                lights[i]->energy, lights[i]->radius, lights[i]->spot_size,
                lights[i]->spot_blend, lights[i]->area_size, lights[i]->sun_angle);
            lp += written; remaining -= (size_t)written;
        }
        snprintf(lp, remaining, "]");
    }

    /* Comfortably over the worst case (objects_json's own 6144 cap +
     * lights_json's own 3072 cap + face_json's own 256 cap + this
     * format string's own static text) -- 8192 was too tight (real
     * -Wformat-truncation warning, not a false positive: those three
     * sub-buffers really can combine to exceed it). */
    char json[10240];
    snprintf(json, sizeof(json),
        "{"
        "\"objects\":%s,"
        "\"selected_object_id\":%d,"
        "\"physics_gravity\":[0.0,-9.81,0.0],"
        "\"selected_face\":%s,"
        "\"lights\":%s,"
        "\"render_settings\":{\"samples\":%d}"
        "}",
        objects_json,
        sel ? sel->id : -1,
        face_json,
        lights_json,
        g_render_settings.samples);

    net_send_scene_state_reply(&g_ns, req_id, json);
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

    /* Real texture loading for glTF imports (see texture_cache.h's own
     * comment) -- must run AFTER renderer_create/GL context exists
     * (texture_cache_load calls real glGenTextures/glTexImage2D), and
     * BEFORE any halfedge_load_gltf/skinned_mesh_load_gltf call that
     * should actually resolve textures rather than leave every material's
     * texture at 0 (see halfedge_gltf_register_texture_loader's own
     * comment on why this is a function-pointer registration, not a
     * direct #include/call, in the first place). */
    halfedge_gltf_register_texture_loader(texture_cache_load);
    skinned_mesh_register_texture_loader(texture_cache_load);

    /* Real multi-object scene graph (Phase 5, see scene_objects.h) --
     * cleared once here, same convention every other bounded registry
     * (light.c/fracture_body.c/ragdoll.c) already uses. Must run BEFORE
     * spawn_test_mesh_object below, which allocates the first slot. */
    scene_objects_init();
    skinned_scene_objects_init();

    /* Phase 6 node graphs (see phi.md's "Geometry and Animation Nodes")
     * -- cleared once here, same registry-init convention as scene_
     * objects_init/light_system_init above/below. No test graphs are
     * created at startup (unlike spawn_test_mesh_object) -- graphs are
     * always explicitly authored via phi.Graph from Python. */
    phi_graph_system_init();

    /* Real C-level render-pass hooks (see render_hooks.h) -- cleared once
     * here, same registry-init convention as the calls just above. No
     * hooks are registered by the editor itself; this only exists so
     * game/src/ *.c code (Phase 9) has somewhere real to register into. */
    render_hooks_init();

    /* Phase 1 foundation test object — see spawn_test_mesh_object's own
     * comment (also reused by the scene context menu's "Add > Mesh
     * Object", see CTX_ACTION_ADD_MESH, and Asset Browser/File > Load,
     * all of which now ADD a new object rather than replacing this one). */
    spawn_test_mesh_object();

    /* Initial camera vantage point, close enough to the test object's own
     * spawn position to see it -- real Blender-style orbit/pan/zoom
     * navigation now exists (MMB-drag/Ctrl+MMB-drag/wheel over the Scene
     * panel, see cam_orbit_from_start/cam_pan_from_start/scene_zoom_cb
     * above), so this is just the starting pose, not "never touched
     * again" the way it used to be. cam_recompute_pos derives g_cam_pos
     * from g_cam_pivot/g_cam_distance/g_cam_yaw/g_cam_pitch's own initial
     * values (chosen to match this exact vantage point) rather than
     * setting g_cam_pos directly, since g_cam_pos is a derived value now,
     * never an independent source of truth. */
    cam_recompute_pos();

    /* Input */
    input_init(&g_inp);
    input_install_callbacks(&g_inp);

    /* Gamepad/Steam Deck input (Phase 9, see input_gamepad.h) -- real
     * SDL2-backed on native Linux, Emscripten's own Gamepad API on wasm,
     * a real honest stub on win32 (see client/vendor/SDL2/VENDORED.md
     * and each backend file's own comment). */
    phi_gamepad_init();

    /* Console / Python panel */
    pyconsole_init(&g_cs);

    /* Chat panel (see phi.md's "Where AI fits") */
    chat_init(&g_chat);
    phi_mp_init(&mp_stack_top);

    /* Phase 2 physics -- see phi.md's "Bullet Physics via Emscripten". A
     * single large static ground box gives "Enable Physics" (Scene
     * context menu) something to actually land on -- there's no other
     * collidable scene geometry (the octree world is gone, see the
     * Client/server model note in phi.md's Phase 1 status). Ground top
     * surface sits at y=50 (center 40, half-extent 10), well below the
     * test object's y=100 default spawn height, so enabling physics on
     * it is an actually-visible fall, not an instant no-op. Created
     * BEFORE phi_mp_register_targets below, since that hands the world
     * pointer off for phi.enable_physics/apply_impulse/etc. to use. */
    g_phys_world = phi_physics_world_create();
    {
        float identity_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        Vec3f ground_half = {200.0f, 10.0f, 200.0f};
        Vec3f ground_pos  = {128.0f, 40.0f, 90.0f};
        g_ground_phys_body = phi_physics_add_box_body(g_phys_world, ground_half, ground_pos, identity_quat, 0.0f, 0.3f);
    }

    /* DNA/RNA + physics targets -- see phi.md's "Property System (DNA/RNA
     * analogue)". phi.prop_get/set('object'/'face', ...) and phi.
     * enable_physics/apply_impulse/get_velocity/set_velocity all resolve
     * "the object" through selected_mesh_object (Phase 5's real multi-
     * object scene graph, see scene_objects.h) every call, not a fixed
     * pointer; g_edit_face is still passed by address (changes every
     * frame) -- phi_mp_register_targets only runs once, here. */
    phi_mp_register_targets(selected_mesh_object, &g_edit_face, g_phys_world);
    /* Shared "object"/"face"/"light:<id>"/"render" resolver (scene_target.h)
     * -- same selected_mesh_object resolver phi_mp_register_targets just
     * got, plus g_render_settings (light:<id> needs no registration, see
     * light.h's own self-contained registry). Used by phi.prop_get/set
     * AND chat-driven scene mutation. */
    scene_target_register(selected_mesh_object, &g_edit_face, &g_render_settings);
    /* phi.render() -- Phase 3's real path tracer (path_tracer.h), same
     * "give mp_port.c a function pointer to main.c's own logic" shape as
     * the registration calls just above. */
    phi_mp_register_render_callback(render_still_frame_to_disk);
    phi_mp_register_ragdoll_callback(activate_ragdoll_on_test_object);
    phi_mp_register_animation_callbacks(anim_play_clip, anim_set_playing_state, anim_get_playing_state,
                                         anim_get_playback_time, anim_set_playback_time, anim_list_clips);

    /* Light objects (Phase 3's "Both like Blender does it" light-source
     * model, see light.h) -- a fixed-capacity registry, cleared once here. */
    light_system_init();

    /* Runtime fracture-activation registry (Phase 2's "shatter on
     * impact" completion, see fracture_body.h) -- cleared once here. */
    fracture_body_system_init();

    /* Phase 4's Armature -> Bullet ragdoll registry (see ragdoll.h) --
     * cleared once here, same convention as the two systems above. */
    ragdoll_system_init();

    /* Phase 4's GPU-skinning + Timeline demonstration object (see
     * g_skinned_test_obj's own comment) -- loaded once at startup, same
     * "auto-load a real fixture" precedent spawn_test_mesh_object()
     * already established for g_test_mesh_object, just a separate slot. */
    g_skinned_test_loaded = skinned_mesh_object_load("assets/test/armature_test.gltf", (Vec3f){170.0f, 55.0f, 90.0f}, &g_skinned_test_obj);
    /* skinned_mesh_object_load itself deliberately auto-starts playback
     * (looped) -- a real, tested library-level default (skinned_mesh_
     * object_test's own "first clip auto-starts playing" check), the
     * right behavior for e.g. a freshly-created/loaded object in
     * general. Startup specifically should NOT begin mid-animation
     * though, per explicit request ("the app needs to start up with it
     * paused") -- a main.c/UX decision, not a library default, so it's
     * overridden right here rather than changing what skinned_mesh_
     * object_load itself does for every caller. Pausing here (not
     * skipping playback.time back to 0 too) means the FIRST frame drawn
     * still shows the clip's real rest-pose-adjacent starting frame
     * (time is already 0 from anim_playback_play), just held rather
     * than advancing until Play is pressed in the Timeline panel. */
    if (g_skinned_test_loaded) g_skinned_test_obj.playback.playing = 0;
    printf("[main] skinned test object: %s\n", g_skinned_test_loaded ? "loaded" : "FAILED to load");

    /* Asset Browser -- see phi.md's "Asset tracking and the Asset Browser
     * panel". net.c requests the initial asset list itself, right after
     * the HELLO handshake completes (see net_send_hello's call sites) --
     * that's the one moment guaranteed to be "actually connected" across
     * all three platforms (native/win32 connect synchronously before
     * main_loop starts; wasm's connection is async, so doing it here
     * instead would silently drop the request if the socket wasn't open
     * yet on the first frame). */
    asset_browser_init(&g_ab);
    asset_browser_set_target(&g_ab);
    net_set_scene_state_handler(scene_state_handler);
    net_set_prop_set_handler(prop_set_handler);
    net_set_add_light_handler(add_light_handler);
    net_set_delete_light_handler(delete_light_handler);
    net_set_create_mesh_handler(create_mesh_handler);
    net_set_set_vertices_handler(set_vertices_handler);
    net_set_delete_mesh_object_handler(delete_mesh_object_handler);
    net_set_get_mesh_vertices_handler(get_mesh_vertices_handler);
    net_set_get_mesh_faces_handler(get_mesh_faces_handler);
    net_set_add_mesh_vertex_handler(add_mesh_vertex_handler);
    net_set_add_mesh_face_handler(add_mesh_face_handler);
    net_set_set_mesh_vertex_handler(set_mesh_vertex_handler);

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
