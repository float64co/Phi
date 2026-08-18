/* A minimal first-person shooter, demonstrating `make player`'s
 * game/src/main.c C-entry path (see phi.md's "./game/ directory" section
 * and client/player_main.c's own top comment: this file's presence is
 * what switches `make player` to real exported C symbols -- game_init/
 * game_tick/game_shutdown below -- with NO MicroPython round trip
 * anywhere in the hot path, requested explicitly for speed). Everything
 * here is a real, already-proven engine call, just reached directly
 * through phi.h instead of through phi.* Python bindings -- the exact
 * same halfedge_build_from_triangles/scene_object_add/meshobject_
 * build_render_mesh_from_halfedge/halfedge_set_face_material sequence
 * mp_port.c's phi.create_mesh/set_face_material make, and the same
 * phi_physics_add_box_body/apply_impulse Bullet calls phi.enable_physics/
 * apply_impulse make.
 *
 * WASD moves on the ground plane (a free-fly camera at a fixed eye
 * height, not a physics-driven player capsule -- a real, deliberate
 * scope cut: a proper walking controller needs capsule sweeps/ground
 * detection/gravity, a separate, larger piece of work, not attempted for
 * this demo). Mouse motion looks around via real relative motion
 * (InputState::mouse_dx/dy) under real OS/browser pointer capture
 * (input_capture_mouse, see input.h) -- click to engage, Escape to
 * release, handled by player_main.c's own policy, not this file. Left
 * click shoots: a hand-rolled ray-vs-sphere hit test against each
 * target's known position/radius (phi_physics.h has no raycast query, so
 * this doesn't need one -- the targets are this file's own objects, their
 * positions are already known), then a real Bullet impulse knocks the
 * hit target flying. Targets that fall off the world get reset to their
 * spawn point, so the range keeps working indefinitely. */
#include "phi.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define N_TARGETS      6
#define MOUSE_SENS     0.0028f
#define MOVE_SPEED     7.0f
#define EYE_HEIGHT     1.7f
#define TARGET_RADIUS  1.35f
#define SHOOT_RANGE    200.0f
#define SHOOT_IMPULSE  16.0f
#define RESPAWN_BELOW_Y -25.0f

typedef struct {
    MeshObject *obj;
    Vec3f       spawn_pos;
} Target;

static Renderer        *g_renderer;
static PhiPhysicsWorld  *g_world;
static const InputState *g_input;

static Vec3f g_cam_pos = { 0.0f, EYE_HEIGHT, 10.0f };
static float g_yaw = 0.0f, g_pitch = 0.0f;
static int   g_shots_fired = 0, g_hits = 0;

static Target g_targets[N_TARGETS];

static Vec3f vec3_sub(Vec3f a, Vec3f b) { return (Vec3f){ a.x-b.x, a.y-b.y, a.z-b.z }; }
static float vec3_dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec3f vec3_scale(Vec3f a, float s) { return (Vec3f){ a.x*s, a.y*s, a.z*s }; }

/* Same yaw/pitch -> forward/right basis editor_main.c's own cam_basis and
 * renderer.c's build_vp/mat4_look_dir use -- kept consistent here too, so
 * "where the camera is looking" and "which way W walks" never disagree. */
static void cam_basis(float yaw, float pitch, Vec3f *fwd, Vec3f *right) {
    float sy = sinf(yaw), cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    if (fwd)   *fwd   = (Vec3f){ -sy*cp, sp, -cy*cp };
    if (right) *right = (Vec3f){ cy, 0.0f, -sy };
}

/* Builds a real box MeshObject from scratch -- the identical real
 * geometry pipeline phi.create_mesh/set_face_material drive from Python
 * (see this file's own top comment), called directly here instead. */
static MeshObject *make_box(float hx, float hy, float hz, Vec3f pos, Vec3f color) {
    float positions[24] = {
        -hx,-hy,-hz,  hx,-hy,-hz,  hx,hy,-hz,  -hx,hy,-hz,
        -hx,-hy, hz,  hx,-hy, hz,  hx,hy, hz,  -hx,hy, hz,
    };
    /* Outward-wound (verified by hand, same convention game/main.py's own
     * _box() helper had to fix after a real, live winding bug found
     * earlier in this project's history -- see phi.md's Phase 9 notes). */
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
    float emission[3] = { 0.0f, 0.0f, 0.0f };
    for (int f = 0; f < hem->face_count; f++)
        halfedge_set_face_material(hem, f, base_color, 0.0f, 0.6f, emission);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    return obj;
}

static void attach_box_physics(MeshObject *obj, Vec3f half_extents, float mass, float restitution) {
    float identity_quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    obj->phys_body = phi_physics_add_box_body(g_world, half_extents, obj->position, identity_quat, mass, restitution);
}

void game_init(Renderer *renderer, PhiPhysicsWorld *phys_world, const InputState *input) {
    g_renderer = renderer;
    g_world = phys_world;
    g_input = input;

    renderer_set_camera(g_renderer, g_cam_pos, g_yaw, g_pitch);

    /* Ground: a large, static (mass=0) box -- top surface at y=0 (center
     * -1, half-extent 1), matching where targets below are spawned to
     * rest. */
    MeshObject *ground = make_box(40.0f, 1.0f, 40.0f, (Vec3f){0.0f, -1.0f, 0.0f}, (Vec3f){0.15f, 0.32f, 0.15f});
    attach_box_physics(ground, (Vec3f){40.0f, 1.0f, 40.0f}, 0.0f, 0.3f);

    /* N_TARGETS real, dynamic physics targets, spread out in front of the
     * camera's default facing direction (yaw=0 -> looking toward -Z),
     * spawned a few units up so they visibly drop and settle onto the
     * ground the moment the game starts -- real, live proof physics is
     * actually running, same "watch it fall" moment game/main.py's own
     * drop box gives the Python path. */
    for (int i = 0; i < N_TARGETS; i++) {
        float x = -7.5f + (float)i * 3.0f;
        float z = -10.0f - 3.0f * (float)(i % 2);
        Vec3f spawn = { x, 4.0f, z };
        MeshObject *t = make_box(0.8f, 0.8f, 0.8f, spawn, (Vec3f){0.85f, 0.25f, 0.15f});
        attach_box_physics(t, (Vec3f){0.8f, 0.8f, 0.8f}, 1.0f, 0.35f);
        g_targets[i].obj = t;
        g_targets[i].spawn_pos = (Vec3f){ x, 4.0f, z };
    }

    printf("[game] FPS demo ready -- WASD to move, mouse to look, click to shoot %d targets\n", N_TARGETS);
}

/* Ray (g_cam_pos + fwd*t) vs. each target's bounding sphere (TARGET_RADIUS)
 * -- phi_physics.h has no raycast query, so this is a real, small, self-
 * contained hit test rather than an engine gap this demo needed to wait
 * on: every target's position is already this file's own data. Returns
 * the nearest hit target index, or -1. */
static int find_shot_target(Vec3f origin, Vec3f fwd) {
    int best = -1;
    float best_t = SHOOT_RANGE;
    for (int i = 0; i < N_TARGETS; i++) {
        if (!g_targets[i].obj->phys_body) continue;
        Vec3f to_target = vec3_sub(g_targets[i].obj->position, origin);
        float t = vec3_dot(to_target, fwd);
        if (t <= 0.0f || t >= best_t) continue;
        Vec3f closest = vec3_sub(vec3_scale(fwd, t), to_target);   /* closest-point-on-ray minus target = perpendicular offset */
        float perp_dist_sq = vec3_dot(closest, closest);
        if (perp_dist_sq <= TARGET_RADIUS * TARGET_RADIUS) {
            best = i;
            best_t = t;
        }
    }
    return best;
}

void game_tick(float dt) {
    /* ---- Mouse look: real relative motion (InputState::mouse_dx/dy --
     * native: XGrabPointer-confined + warped-to-center each frame;
     * wasm: the browser's real Pointer Lock API's movementX/Y), not a
     * hand-diffed absolute-position delta -- input_capture_mouse (see
     * player_main.c's own click-to-engage/Escape-to-release policy) is
     * what makes this both accurate (no window-edge clamping) and
     * literally "steal the mouse until Escape", real OS/browser-level
     * pointer capture, not a cosmetic hidden-cursor illusion. ---- */
    g_yaw   -= (float)g_input->mouse_dx * MOUSE_SENS;
    g_pitch -= (float)g_input->mouse_dy * MOUSE_SENS;
    if (g_pitch > 1.5f) g_pitch = 1.5f;
    if (g_pitch < -1.5f) g_pitch = -1.5f;

    /* ---- WASD, on the flat ground plane (pitch=0 for movement so
     * looking up/down doesn't slow horizontal walking -- standard FPS
     * convention). ---- */
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

    /* ---- Click to shoot -- lmb_click is a real one-shot edge, drained
     * once per frame by player_main.c after game_tick returns (a real
     * fix landed alongside this demo: nothing was consuming it before,
     * so it would have latched permanently true after the first click). ---- */
    if (g_input->lmb_click) {
        g_shots_fired++;
        Vec3f fwd;
        cam_basis(g_yaw, g_pitch, &fwd, NULL);
        int hit = find_shot_target(g_cam_pos, fwd);
        if (hit >= 0) {
            g_hits++;
            phi_physics_apply_impulse(g_targets[hit].obj->phys_body, vec3_scale(fwd, SHOOT_IMPULSE), (Vec3f){0,0,0});
            printf("[game] hit target %d! (%d/%d shots landed)\n", hit, g_hits, g_shots_fired);
        } else {
            printf("[game] miss (%d/%d shots landed)\n", g_hits, g_shots_fired);
        }
    }

    /* ---- Targets that fall off the world reset to their spawn point --
     * keeps the range usable indefinitely instead of it slowly emptying
     * out. A real per-frame physics-state check/reset from pure C. ---- */
    for (int i = 0; i < N_TARGETS; i++) {
        MeshObject *t = g_targets[i].obj;
        if (t->phys_body && t->position.y < RESPAWN_BELOW_Y) {
            float identity_quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            phi_physics_set_transform(t->phys_body, g_targets[i].spawn_pos, identity_quat);
            phi_physics_set_linear_velocity(t->phys_body, (Vec3f){0.0f, 0.0f, 0.0f});
        }
    }
}

void game_shutdown(void) {
    printf("[game] shutting down -- %d/%d shots landed\n", g_hits, g_shots_fired);
}
