/* Tour De Force II -- a first pass at the "generic red vs. blue thing"
 * requested for this project: DISA's swat_operator squad (blue) facing
 * off against a Rushab cyberdemon (notfreedom, red), both real, fully
 * textured Sketchfab imports loaded through this session's new multi-
 * mesh/multi-material glTF pipeline (see halfedge_gltf.c/skinned_mesh.c's
 * own top comments), scaled to a common height, standing on a ground
 * plane the player can walk around on and look at them from any angle
 * (WASD + mouse look, same free-fly camera client/main.c's own earlier
 * FPS demo established -- see this file's git history).
 *
 * Two honest, deliberate scope notes, not silently papered over:
 *
 *   1. swat_operator has a real skin + 1 real animation clip in its own
 *      glTF file -- skinned_mesh_object_load auto-starts it looping, so
 *      it's genuinely, correctly animated here, real GPU vertex skinning,
 *      not faked.
 *   2. notfreedom (the cyberdemon) has ZERO skins and ZERO animations in
 *      its own source file -- there is no skeleton to animate, full stop,
 *      no amount of engine work on this project's side changes that. The
 *      substitute here is a slow, honest whole-object idle sway (a Y-axis
 *      turn), not a claim of real skeletal animation it doesn't have. A
 *      future pass could rig/animate a new skeleton for it in a DCC tool
 *      and re-export, but that's new content work, not an engine gap.
 *
 * No shooting/physics-target mechanics here (this file's earlier FPS-demo
 * incarnation had some against plain colored boxes) -- this pass is
 * specifically about getting the two real characters loaded, textured,
 * animated (where the source data allows it), and viewable together;
 * combat mechanics against them are real, separate future work. */
#include "phi.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define MOUSE_SENS   0.0004f
#define MOVE_SPEED   6.0f
#define EYE_HEIGHT   1.7f
#define TARGET_HEIGHT_M 1.85f   /* common height (in world units, meters) both characters are scaled to */
#define CYBERDEMON_SWAY_SPEED 0.6f   /* rad/s -- see this file's own top comment on why this exists at all */
#define CYBERDEMON_SWAY_AMPLITUDE 0.35f   /* radians */

static Renderer        *g_renderer;
static const InputState *g_input;

static Vec3f g_cam_pos = { 0.0f, EYE_HEIGHT, 8.0f };
static float g_yaw = 0.0f, g_pitch = 0.0f;
/* Clamped just shy of +/-90 degrees (see game_tick) -- straight up/down
 * puts cam_basis's fwd vector on the vertical axis, which degenerates
 * yaw (sy/cy stop mattering) rather than actually breaking anything, but
 * clamping short of it keeps mouse-look feeling like a normal FPS camera
 * instead of letting it wrap/flip past vertical. */
#define PITCH_LIMIT 1.5f

static SkinnedMeshObject *g_swat;      /* blue: DISA swat_operator, real skin + animation */
static MeshObject        *g_cyberdemon; /* red: Rushab notfreedom, static (no skin in source) */
static float g_cyberdemon_base_yaw;
static float g_sway_t = 0.0f;

static void cam_basis(float yaw, float pitch, Vec3f *fwd, Vec3f *right) {
    float sy = sinf(yaw), cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    if (fwd)   *fwd   = (Vec3f){ -sy*cp, sp, -cy*cp };
    if (right) *right = (Vec3f){ cy, 0.0f, -sy };
}

static MeshObject *make_ground(float hx, float hz) {
    float hy = 1.0f;
    float positions[24] = {
        -hx,-hy,-hz,  hx,-hy,-hz,  hx,hy,-hz,  -hx,hy,-hz,
        -hx,-hy, hz,  hx,-hy, hz,  hx,hy, hz,  -hx,hy, hz,
    };
    unsigned short indices[36] = {
        0,2,1,  0,3,2,
        5,7,4,  5,6,7,
        4,3,0,  4,7,3,
        1,6,5,  1,2,6,
        3,6,2,  3,7,6,
        4,1,5,  4,0,1,
    };
    HalfEdgeMesh *hem = halfedge_build_from_triangles(positions, 8, indices, 36);
    MeshObject *obj = scene_object_add();
    obj->position = (Vec3f){0.0f, -1.0f, 0.0f};
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj->hem = hem;
    obj->render_mesh = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    float base_color[3] = { 0.15f, 0.16f, 0.18f };
    float emission[3] = { 0.0f, 0.0f, 0.0f };
    for (int f = 0; f < hem->face_count; f++)
        halfedge_set_face_material(hem, f, base_color, 0.0f, 0.85f, emission);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    return obj;
}

/* A small, real emissive cube (self-glowing -- see HEFace::emission's own
 * comment in halfedge.h) paired with a real point light of the same
 * color at the same position (see gbuffer_set_point_lights' own comment
 * in gbuffer.h/player_main.c) -- the cube's own emission alone would only
 * make ITSELF glow, this renderer's deferred lighting pass has no real-
 * time global illumination to carry that light onto nearby geometry, so
 * the point light is what actually illuminates the two characters
 * standing on either side of it. */
static MeshObject *make_emissive_cube(Vec3f pos, float half_extent, Vec3f color, float point_light_energy) {
    float h = half_extent;
    float positions[24] = {
        -h,-h,-h,  h,-h,-h,  h,h,-h,  -h,h,-h,
        -h,-h, h,  h,-h, h,  h,h, h,  -h,h, h,
    };
    unsigned short indices[36] = {
        0,2,1,  0,3,2,
        5,7,4,  5,6,7,
        4,3,0,  4,7,3,
        1,6,5,  1,2,6,
        3,6,2,  3,7,6,
        4,1,5,  4,0,1,
    };
    HalfEdgeMesh *hem = halfedge_build_from_triangles(positions, 8, indices, 36);
    MeshObject *obj = scene_object_add();
    obj->position = pos;
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj->hem = hem;
    obj->render_mesh = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    float base_color[3] = { color.x, color.y, color.z };
    /* A high emission value (well above 1.0, real HDR) so it visibly
     * glows/blooms rather than just reading as a flat-lit colored box --
     * see HEFace::emission's own comment on real, above-1.0 emissive
     * surfaces being expected here. */
    float emission[3] = { color.x * 8.0f, color.y * 8.0f, color.z * 8.0f };
    for (int f = 0; f < hem->face_count; f++)
        halfedge_set_face_material(hem, f, base_color, 0.0f, 0.4f, emission);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);

    PhiLight *pl = light_spawn(LIGHT_TYPE_POINT, pos);
    if (pl) {
        pl->color = color;
        pl->energy = point_light_energy;
    } else {
        printf("[game] WARNING: light registry full, the emissive cube won't actually light anything\n");
    }
    return obj;
}

void game_init(Renderer *renderer, PhiPhysicsWorld *phys_world, const InputState *input) {
    (void)phys_world;
    g_renderer = renderer;
    g_input = input;

    renderer_set_camera(g_renderer, g_cam_pos, g_yaw, g_pitch);

    make_ground(30.0f, 30.0f);

    /* Ground's own top surface is at world Y=0 (position.y=-1, half-
     * extent 1 -- see make_ground above), NOT position.y=-1 itself -- both
     * characters below are placed with that in mind: their own local
     * lowest vertex (not their local origin, which a Sketchfab export can
     * put anywhere -- hips, pelvis, world origin, wherever the original
     * scene had it) is what actually gets planted at world Y=0, via
     * min_y*scale below. Placing them at a flat "position.y=-1" instead
     * (an earlier, real bug in this file) silently assumed local-origin-
     * at-feet and sank both characters up to a meter into the ground --
     * caught after a live run put the noclip camera (no collision, no
     * back-face culling -- see renderer_create's own comment) inside the
     * exposed geometry, which reads as "the view is stuck inside solid
     * mesh" from the player's side. */

    /* ---- Blue: DISA swat_operator, real skin + animation ---- */
    g_swat = skinned_scene_object_add();
    if (g_swat && skinned_mesh_object_load("game/assets/swat_operator/scene.gltf", (Vec3f){-3.0f, 0.0f, 0.0f}, g_swat)) {
        /* Measured against the ACTUAL skinned pose (skinned_mesh_object_
         * local_aabb), not raw bind-pose vertex data -- see its own
         * comment on why a raw scan would be wrong here. */
        Vec3f mn, mx;
        float scale = 1.0f;
        float min_y = 0.0f;
        if (skinned_mesh_object_local_aabb(g_swat, &mn, &mx) && (mx.y - mn.y) > 1e-4f) {
            scale = TARGET_HEIGHT_M / (mx.y - mn.y);
            min_y = mn.y;
        }
        g_swat->scale = (Vec3f){scale, scale, scale};
        g_swat->position.y = -min_y * scale;
        g_swat->orientation = quat_identity();
        printf("[game] blue: swat_operator loaded, %d bones, %d clip(s), scaled x%.3f, feet at y=%.3f\n",
               g_swat->arm.bone_count, g_swat->clip_count, scale, g_swat->position.y);
    } else {
        printf("[game] blue: FAILED to load swat_operator\n");
        g_swat = NULL;
    }

    /* ---- Red: Rushab cyberdemon (notfreedom), static (no skin in source) ---- */
    HalfEdgeMesh *cyber_hem = halfedge_load_gltf("game/assets/notfreedom/scene.gltf");
    if (cyber_hem) {
        Vec3f half;
        float scale = 1.0f;
        float min_y = 0.0f;
        if (meshobject_local_aabb_half_extents(cyber_hem, &half) && half.y > 1e-4f) {
            scale = TARGET_HEIGHT_M / (half.y * 2.0f);
            min_y = cyber_hem->verts[0].pos[1];
            for (int i = 1; i < cyber_hem->vert_count; i++)
                if (cyber_hem->verts[i].pos[1] < min_y) min_y = cyber_hem->verts[i].pos[1];
        }

        g_cyberdemon = scene_object_add();
        g_cyberdemon->position = (Vec3f){3.0f, -min_y * scale, 0.0f};
        /* Both characters spawn at their own file's neutral orientation
         * (yaw 0, no extra rotation on top of it) rather than turned to
         * face each other -- the request is "facing the camera", and the
         * camera sits on the same +Z side both Sketchfab assets were
         * almost certainly PHOTOGRAPHED/exported facing (that's the
         * standard convention for a marketplace preview shot). This is a
         * real, reasoned bet, not a verified fact -- I can't render a
         * screenshot to confirm it myself. If either character turns out
         * to be facing away once you look, tell me which one and I'll
         * add a single 180-degree yaw flip for it. */
        g_cyberdemon_base_yaw = 0.0f;
        g_cyberdemon->orientation = quat_identity();
        g_cyberdemon->scale = (Vec3f){scale, scale, scale};
        g_cyberdemon->hem = cyber_hem;
        g_cyberdemon->render_mesh = (RenderMesh *)calloc(1, sizeof(RenderMesh));
        meshobject_build_render_mesh_from_halfedge(g_cyberdemon->render_mesh, cyber_hem);
        printf("[game] red: notfreedom (cyberdemon) loaded, no skin in source -- using idle sway, scaled x%.3f, feet at y=%.3f\n",
               scale, g_cyberdemon->position.y);
    } else {
        printf("[game] red: FAILED to load notfreedom\n");
        g_cyberdemon = NULL;
    }

    /* A real sun (light.h) -- direction only (this codebase's own real-
     * time lighting pass doesn't yet read a sun's color/energy, just its
     * direction -- see player_main.c's own light-gathering comment), a
     * bit higher and more front-on than the engine's previous hardcoded
     * default so it actually rakes across both characters' faces instead
     * of grazing them from directly behind. */
    PhiLight *sun = light_spawn(LIGHT_TYPE_SUN, (Vec3f){0.0f, 0.0f, 0.0f});
    if (sun) sun->direction = (Vec3f){0.3f, 0.8f, 0.5f};

    /* A small emissive cube between the two characters (x=0, the midpoint
     * of their x=-3/x=3 spawn points), roughly chest-height, paired with
     * a real point light at the same spot -- see make_emissive_cube's own
     * comment on why both are needed for it to actually "light them both
     * up" rather than just glow on its own. */
    make_emissive_cube((Vec3f){0.0f, 1.0f, 0.0f}, 0.15f, (Vec3f){1.0f, 0.75f, 0.35f}, 900.0f);

    printf("[game] Tour De Force II -- WASD to move, mouse to look\n");
}

void game_tick(float dt) {
    /* ---- Mouse look: real relative motion, both axes (see input.h's own
     * mouse_dx/dy comments) -- pitch clamped to +/-PITCH_LIMIT (see its
     * own comment) so looking straight up/down doesn't flip the camera
     * past vertical. ---- */
    g_yaw   -= (float)g_input->mouse_dx * MOUSE_SENS;
    g_pitch -= (float)g_input->mouse_dy * MOUSE_SENS;
    if (g_pitch > PITCH_LIMIT) g_pitch = PITCH_LIMIT;
    if (g_pitch < -PITCH_LIMIT) g_pitch = -PITCH_LIMIT;

    /* WASD movement stays on the flat ground plane regardless of pitch
     * (cam_basis(g_yaw, 0.0f, ...) below, not g_pitch) -- standard FPS
     * convention: looking up/down doesn't slow or tilt horizontal
     * walking. */
    Vec3f fwd_flat, right;
    cam_basis(g_yaw, 0.0f, &fwd_flat, &right);
    float mx = 0.0f, mz = 0.0f;
    if (g_input->keys_down[PHI_KEY_W]) { mx += fwd_flat.x; mz += fwd_flat.z; }
    if (g_input->keys_down[PHI_KEY_S]) { mx -= fwd_flat.x; mz -= fwd_flat.z; }
    if (g_input->keys_down[PHI_KEY_D]) { mx += right.x;    mz += right.z; }
    if (g_input->keys_down[PHI_KEY_A]) { mx -= right.x;    mz -= right.z; }
    float mlen = sqrtf(mx*mx + mz*mz);
    if (mlen > 1e-5f) {
        g_cam_pos.x += (mx / mlen) * MOVE_SPEED * dt;
        g_cam_pos.z += (mz / mlen) * MOVE_SPEED * dt;
    }
    renderer_set_camera(g_renderer, g_cam_pos, g_yaw, g_pitch);

    /* ---- The cyberdemon's idle sway (see this file's own top comment on
     * why this exists instead of real skeletal animation) -- a slow,
     * honest yaw oscillation around its base facing, real per-frame
     * motion, not a static prop. ---- */
    if (g_cyberdemon) {
        g_sway_t += dt;
        float yaw = g_cyberdemon_base_yaw + sinf(g_sway_t * CYBERDEMON_SWAY_SPEED) * CYBERDEMON_SWAY_AMPLITUDE;
        g_cyberdemon->orientation = (Quat){0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
    }
    /* g_swat's own animation clip advances automatically every frame via
     * player_main.c's skinned_scene_object_get_all/skinned_mesh_object_
     * update loop (see skinned_scene_objects.h) -- nothing to do here. */
}

void game_shutdown(void) {
    printf("[game] shutting down\n");
}
