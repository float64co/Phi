#include "ragdoll.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RAGDOLL_MAX_CONSTRAINTS RAGDOLL_MAX_BONES   /* at most one parent-link per non-root bone */

typedef struct {
    PhiRigidBody *body;
    Vec3f         center;        /* world-space capsule center at spawn time -- used to compute constraint pivots */
    Quat          orientation;   /* world-space capsule orientation at spawn time */
} RagdollBody;

/* bone index -> slot in s_bodies[], or -1 if that bone didn't get a body
 * (shouldn't happen this pass -- every bone gets one -- but mirrors
 * fracture_body.c's own slot_of[] defensive pattern). */
static int          s_slot_of[RAGDOLL_MAX_BONES];
static RagdollBody   s_bodies[RAGDOLL_MAX_BONES];
static int           s_body_count = 0;
static PhiConstraint *s_constraints[RAGDOLL_MAX_CONSTRAINTS];
static int            s_constraint_count = 0;

void ragdoll_system_init(void) {
    memset(s_bodies, 0, sizeof(s_bodies));
    s_body_count = 0;
    memset(s_constraints, 0, sizeof(s_constraints));
    s_constraint_count = 0;
}

/* ---- local vec3/quat helpers, same per-file convention as fracture_
 * body.c/path_tracer.c's own local copies rather than a shared header. ---- */
static Vec3f v3add(Vec3f a, Vec3f b) { return (Vec3f){a.x+b.x, a.y+b.y, a.z+b.z}; }
static Vec3f v3sub(Vec3f a, Vec3f b) { return (Vec3f){a.x-b.x, a.y-b.y, a.z-b.z}; }
static Vec3f v3scale(Vec3f a, float s) { return (Vec3f){a.x*s, a.y*s, a.z*s}; }
static float v3dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec3f v3cross(Vec3f a, Vec3f b) {
    return (Vec3f){ a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
}
static float v3len(Vec3f a) { return sqrtf(v3dot(a,a)); }
static Vec3f v3norm(Vec3f a) {
    float l = v3len(a);
    return (l > 1e-8f) ? v3scale(a, 1.0f/l) : (Vec3f){0,1,0};
}

/* Shortest-arc rotation taking world +Y to `dir` (unit) -- standard
 * construction (axis = cross(Y,dir), w = 1+dot(Y,dir), normalize), not
 * derived here. Handles the near-180-degree case (dir close to -Y)
 * separately since the general formula's w->0 there is numerically
 * degenerate, not wrong -- picks an arbitrary perpendicular axis for
 * that specific case, same standard fix every quaternion-from-vectors
 * implementation needs. */
static Quat quat_from_y_to(Vec3f dir) {
    Vec3f from = {0.0f, 1.0f, 0.0f};
    Vec3f d = v3norm(dir);
    float dot = v3dot(from, d);
    if (dot > 0.9999f) return quat_identity();
    if (dot < -0.9999f) {
        Vec3f axis = v3cross((Vec3f){1,0,0}, from);
        if (v3len(axis) < 1e-6f) axis = v3cross((Vec3f){0,0,1}, from);
        axis = v3norm(axis);
        return (Quat){axis.x, axis.y, axis.z, 0.0f};
    }
    Vec3f axis = v3cross(from, d);
    Quat q = { axis.x, axis.y, axis.z, 1.0f + dot };
    return quat_normalize(q);
}

/* world -> obj's local space for a POINT (composes obj's own position +
 * orientation + scale, matching renderer_draw_skinned_mesh's model
 * matrix T*(R*S) exactly). */
static Vec3f object_transform_point(const SkinnedMeshObject *obj, Vec3f local) {
    float rot[16]; quat_to_mat4(&obj->orientation, rot);
    Vec3f sc = { local.x*obj->scale.x, local.y*obj->scale.y, local.z*obj->scale.z };
    Vec3f rotated = {
        rot[0]*sc.x + rot[4]*sc.y + rot[8]*sc.z,
        rot[1]*sc.x + rot[5]*sc.y + rot[9]*sc.z,
        rot[2]*sc.x + rot[6]*sc.y + rot[10]*sc.z
    };
    return v3add(rotated, obj->position);
}

/* Same, for a DIRECTION (rotation only, no translate/scale -- used only
 * by the leaf-bone fallback below to find "which way this bone points",
 * where exactness under non-uniform scale doesn't matter, only the
 * general direction does). */
static Vec3f object_transform_dir(const SkinnedMeshObject *obj, Vec3f local_dir) {
    float rot[16]; quat_to_mat4(&obj->orientation, rot);
    return (Vec3f){
        rot[0]*local_dir.x + rot[4]*local_dir.y + rot[8]*local_dir.z,
        rot[1]*local_dir.x + rot[5]*local_dir.y + rot[9]*local_dir.z,
        rot[2]*local_dir.x + rot[6]*local_dir.y + rot[10]*local_dir.z
    };
}

static Vec3f mat4_translation(const float m[16]) { return (Vec3f){m[12], m[13], m[14]}; }
/* Bone's own local Y axis (column 1) from its armature-space world
 * matrix -- used only by the leaf-bone fallback direction below. */
static Vec3f mat4_col_y(const float m[16]) { return (Vec3f){m[4], m[5], m[6]}; }

/* World point -> a capsule's own local space, for computing a point2point
 * constraint pivot (phi_physics_add_point2point_constraint takes each
 * body's OWN local pivot, see its header comment) -- transpose-as-
 * inverse of the (orthonormal) capsule orientation matrix, same
 * technique meshobject_world_to_local already established. */
static Vec3f world_to_capsule_local(Vec3f world_point, Vec3f center, Quat orientation) {
    float rot[16]; quat_to_mat4(&orientation, rot);
    Vec3f d = v3sub(world_point, center);
    return (Vec3f){
        rot[0]*d.x + rot[1]*d.y + rot[2]*d.z,
        rot[4]*d.x + rot[5]*d.y + rot[6]*d.z,
        rot[8]*d.x + rot[9]*d.y + rot[10]*d.z
    };
}

int ragdoll_activate(PhiPhysicsWorld *world, const SkinnedMeshObject *obj,
                      float mass_per_bone, float restitution) {
    ragdoll_clear(world);
    if (!obj || obj->arm.bone_count < 1) return 0;

    int n = obj->arm.bone_count;
    if (n > RAGDOLL_MAX_BONES) n = RAGDOLL_MAX_BONES;

    Vec3f true_head[RAGDOLL_MAX_BONES];
    for (int i = 0; i < n; i++) {
        s_slot_of[i] = -1;
        true_head[i] = object_transform_point(obj, mat4_translation(obj->world[i]));
    }

    int spawned = 0;
    for (int i = 0; i < n; i++) {
        /* Find this bone's first child (see this file's header comment
         * on why "first child" is a reasonable, real choice -- a bone
         * with multiple children, e.g. a torso with two arms, has no
         * single unambiguous "tail" anyway; a real multi-child rig would
         * need per-limb ragdoll authoring, out of scope here). */
        int child = -1;
        for (int j = 0; j < n; j++) {
            if (obj->arm.bones[j].parent == i) { child = j; break; }
        }

        Vec3f tail;
        if (child >= 0) {
            tail = true_head[child];
        } else {
            /* Leaf bone -- no real tail data. Extend along this bone's
             * own local +Y axis (its own accumulated rotation, rotated
             * into world space by the object's own orientation) by the
             * length of the bone that fed INTO it (a real, common
             * "assume the tip segment is about as long as its parent
             * segment" ragdoll-generation heuristic), or a small fixed
             * fallback (1.0) for the degenerate single-bone-no-parent
             * case. */
            float fallback_len = 1.0f;
            if (obj->arm.bones[i].parent >= 0) {
                fallback_len = v3len(v3sub(true_head[i], true_head[obj->arm.bones[i].parent]));
                if (fallback_len < 1e-4f) fallback_len = 1.0f;
            }
            Vec3f dir = object_transform_dir(obj, mat4_col_y(obj->world[i]));
            tail = v3add(true_head[i], v3scale(v3norm(dir), fallback_len));
        }

        Vec3f seg = v3sub(tail, true_head[i]);
        float length = v3len(seg);
        if (length < 1e-4f) length = 0.1f;   /* degenerate zero-length segment -- still spawn a real, tiny, non-degenerate capsule rather than skipping the bone */
        Vec3f center = v3add(true_head[i], v3scale(seg, 0.5f));
        Quat  orientation = quat_from_y_to(length > 1e-4f ? seg : (Vec3f){0,1,0});

        float radius = 0.15f * length;
        if (radius < 0.05f) radius = 0.05f;
        float half_height = length * 0.5f - radius;
        if (half_height < 0.02f) half_height = 0.02f;

        float orientation4[4] = { orientation.x, orientation.y, orientation.z, orientation.w };
        PhiRigidBody *body = phi_physics_add_capsule_body(world, radius, half_height,
                                                            center, orientation4,
                                                            mass_per_bone, restitution);
        s_bodies[spawned].body = body;
        s_bodies[spawned].center = center;
        s_bodies[spawned].orientation = orientation;
        s_slot_of[i] = spawned;
        spawned++;
    }
    s_body_count = spawned;

    /* One point2point joint per non-root bone, pinning this bone's head
     * to its parent's own body at that same shared world point (each
     * side's pivot expressed in ITS OWN capsule's local space, see
     * world_to_capsule_local). */
    for (int i = 0; i < n; i++) {
        int p = obj->arm.bones[i].parent;
        if (p < 0 || s_slot_of[i] < 0 || s_slot_of[p] < 0) continue;
        if (s_constraint_count >= RAGDOLL_MAX_CONSTRAINTS) break;
        RagdollBody *bi = &s_bodies[s_slot_of[i]];
        RagdollBody *bp = &s_bodies[s_slot_of[p]];
        Vec3f joint_world = true_head[i];
        Vec3f pivot_a = world_to_capsule_local(joint_world, bp->center, bp->orientation);
        Vec3f pivot_b = world_to_capsule_local(joint_world, bi->center, bi->orientation);
        s_constraints[s_constraint_count++] =
            phi_physics_add_point2point_constraint(world, bp->body, pivot_a, bi->body, pivot_b);
    }

    return spawned;
}

void ragdoll_clear(PhiPhysicsWorld *world) {
    for (int i = 0; i < s_constraint_count; i++) {
        phi_physics_remove_constraint(world, s_constraints[i]);
    }
    s_constraint_count = 0;
    for (int i = 0; i < s_body_count; i++) {
        if (s_bodies[i].body) phi_physics_remove_body(world, s_bodies[i].body);
    }
    memset(s_bodies, 0, sizeof(s_bodies));
    s_body_count = 0;
}

int ragdoll_count(void) { return s_body_count; }

int ragdoll_get_transform(int index, Vec3f *out_position, Quat *out_orientation) {
    if (index < 0 || index >= s_body_count || !s_bodies[index].body) return 0;
    float orientation4[4];
    phi_physics_get_transform(s_bodies[index].body, out_position, orientation4);
    if (out_orientation) {
        out_orientation->x = orientation4[0];
        out_orientation->y = orientation4[1];
        out_orientation->z = orientation4[2];
        out_orientation->w = orientation4[3];
    }
    return 1;
}
