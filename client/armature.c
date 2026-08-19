#include "armature.h"
#include "vecmath_simd.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* mat4_mul now comes from vecmath_simd.h (see its own top comment) --
 * GL-free/header-only, so it's safe here despite this module needing to
 * stay linkable into a standalone no-window test harness (animation_
 * test_main.c). SSE2-accelerated on native x86 builds; this is the real
 * per-frame hot path that motivated adding that acceleration in the
 * first place -- see armature_compute_world_transforms/_skinning_
 * matrices below, up to ARMATURE_MAX_BONES calls each, per skinned
 * object, every frame. */

/* Composes a column-major T*R*S matrix directly (equivalent to, but
 * cheaper than, three separate mat4_mul calls) -- standard OpenGL
 * convention, same m[col*4+row] layout quat_to_mat4/renderer.c's mat4_*
 * all use. Scale only ever multiplies R's columns (a diagonal S
 * right-multiplied into R scales each of R's columns by the matching
 * diagonal entry), translation is just T's own last column. */
static void mat4_from_trs(Vec3f t, Quat r, Vec3f s, float *out) {
    float rot[16];
    quat_to_mat4(&r, rot);
    out[0]  = rot[0]  * s.x; out[1]  = rot[1]  * s.x; out[2]  = rot[2]  * s.x; out[3]  = 0.0f;
    out[4]  = rot[4]  * s.y; out[5]  = rot[5]  * s.y; out[6]  = rot[6]  * s.y; out[7]  = 0.0f;
    out[8]  = rot[8]  * s.z; out[9]  = rot[9]  * s.z; out[10] = rot[10] * s.z; out[11] = 0.0f;
    out[12] = t.x; out[13] = t.y; out[14] = t.z; out[15] = 1.0f;
}

/* Decomposes a column-major TRS-only 4x4 matrix (no shear -- every
 * matrix this is ever called on is either cgltf_node_transform_world's
 * own output, itself a product of pure TRS node transforms, or an
 * identity-seeded product of one, so shear can't arise) into translation/
 * rotation/scale. Standard trace-based (Shepperd's method) rotation-
 * matrix-to-quaternion conversion -- picks whichever of the 4 branches
 * has the largest denominator to avoid dividing by ~0, the well-known
 * numerically-stable form (naively always using the trace>0 branch blows
 * up near 180-degree rotations). Used by armature_load_from_skin below
 * to fold a topmost joint's real ancestor-chain world transform (e.g. a
 * glTF file's own Z-up-to-Y-up corrective root rotation, sitting above
 * the skeleton rather than baked into any joint) into that joint's own
 * rest pose, rather than silently dropping it -- see that function's own
 * comment for why this matters. */
static void mat4_decompose_trs(const float *m, Vec3f *out_t, Quat *out_r, Vec3f *out_s) {
    out_t->x = m[12]; out_t->y = m[13]; out_t->z = m[14];

    float sx = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float sy = sqrtf(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    float sz = sqrtf(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
    out_s->x = sx; out_s->y = sy; out_s->z = sz;
    if (sx < 1e-8f) sx = 1.0f;
    if (sy < 1e-8f) sy = 1.0f;
    if (sz < 1e-8f) sz = 1.0f;

    float r00 = m[0]/sx, r10 = m[1]/sx, r20 = m[2]/sx;
    float r01 = m[4]/sy, r11 = m[5]/sy, r21 = m[6]/sy;
    float r02 = m[8]/sz, r12 = m[9]/sz, r22 = m[10]/sz;

    float trace = r00 + r11 + r22;
    float qx, qy, qz, qw;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        qw = 0.25f * s;
        qx = (r21 - r12) / s;
        qy = (r02 - r20) / s;
        qz = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        float s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        qw = (r21 - r12) / s;
        qx = 0.25f * s;
        qy = (r01 + r10) / s;
        qz = (r02 + r20) / s;
    } else if (r11 > r22) {
        float s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        qw = (r02 - r20) / s;
        qx = (r01 + r10) / s;
        qy = 0.25f * s;
        qz = (r12 + r21) / s;
    } else {
        float s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        qw = (r10 - r01) / s;
        qx = (r02 + r20) / s;
        qy = (r12 + r21) / s;
        qz = 0.25f * s;
    }
    out_r->x = qx; out_r->y = qy; out_r->z = qz; out_r->w = qw;
}

/* Nearest glTF-hierarchy ancestor of `node` that's also one of `joints`
 * (walks node->parent repeatedly, not just one level -- a real rig's
 * joints usually form an unbroken chain among themselves, but this is
 * defensive against a gap). Returns its index into `joints`, or -1 if
 * none (node is effectively a root within this skin). */
static int find_joint_ancestor(cgltf_node *node, cgltf_node **joints, cgltf_size joints_count) {
    cgltf_node *p = node->parent;
    while (p) {
        for (cgltf_size i = 0; i < joints_count; i++) {
            if (joints[i] == p) return (int)i;
        }
        p = p->parent;
    }
    return -1;
}

int armature_load_from_skin(const cgltf_skin *skin, Armature *out) {
    if (!skin || !out) return 0;
    if (skin->joints_count == 0 || skin->joints_count > ARMATURE_MAX_BONES) {
        printf("[armature] load failed: joint count %zu out of range (1..%d)\n",
               skin->joints_count, ARMATURE_MAX_BONES);
        return 0;
    }
    if (!skin->inverse_bind_matrices || skin->inverse_bind_matrices->count != skin->joints_count ||
        skin->inverse_bind_matrices->type != cgltf_type_mat4) {
        printf("[armature] load failed: missing/malformed inverseBindMatrices accessor\n");
        return 0;
    }

    cgltf_size n = skin->joints_count;

    /* Step 1: each joint's parent, expressed as an index into skin->joints
     * (ORIGINAL order) -- not yet topologically sorted. */
    int orig_parent[ARMATURE_MAX_BONES];
    for (cgltf_size i = 0; i < n; i++)
        orig_parent[i] = find_joint_ancestor(skin->joints[i], skin->joints, n);

    /* Step 2: topological sort (parent-before-child) -- repeatedly peel
     * off any not-yet-placed joint whose parent is already placed (or is
     * a root). n <= ARMATURE_MAX_BONES (256) so the O(n^2) worst case is
     * trivial; a tree has no cycles so this always terminates with
     * everything placed, but the placed_count guard below still catches
     * genuinely malformed data (e.g. a real cycle) rather than looping
     * forever. */
    int new_index[ARMATURE_MAX_BONES];   /* orig index -> sorted index */
    int order[ARMATURE_MAX_BONES];       /* sorted index -> orig index */
    int placed[ARMATURE_MAX_BONES];
    memset(placed, 0, sizeof(placed));
    int placed_count = 0;
    while (placed_count < (int)n) {
        int progressed = 0;
        for (cgltf_size i = 0; i < n; i++) {
            if (placed[i]) continue;
            int op = orig_parent[i];
            if (op == -1 || placed[op]) {
                new_index[i] = placed_count;
                order[placed_count] = (int)i;
                placed[i] = 1;
                placed_count++;
                progressed = 1;
            }
        }
        if (!progressed) {
            printf("[armature] load failed: joint parent cycle detected (placed %d/%zu)\n", placed_count, n);
            return 0;
        }
    }

    out->bone_count = (int)n;
    const cgltf_accessor *ibm_acc = skin->inverse_bind_matrices;

    for (cgltf_size sorted_i = 0; sorted_i < n; sorted_i++) {
        int orig_i = order[sorted_i];
        cgltf_node *node = skin->joints[orig_i];
        Bone *b = &out->bones[sorted_i];

        strncpy(b->name, node->name ? node->name : "", BONE_NAME_LEN - 1);
        b->name[BONE_NAME_LEN - 1] = 0;

        b->parent = (orig_parent[orig_i] == -1) ? -1 : new_index[orig_parent[orig_i]];

        b->rest_translation = node->has_translation
            ? (Vec3f){node->translation[0], node->translation[1], node->translation[2]}
            : (Vec3f){0.0f, 0.0f, 0.0f};
        b->rest_rotation = node->has_rotation
            ? (Quat){node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]}
            : quat_identity();
        b->rest_scale = node->has_scale
            ? (Vec3f){node->scale[0], node->scale[1], node->scale[2]}
            : (Vec3f){1.0f, 1.0f, 1.0f};

        /* A topmost joint (b->parent == -1, i.e. its cgltf-level parent
         * isn't itself one of this skin's joints -- see find_joint_
         * ancestor's own comment) can still have a REAL cgltf ancestor
         * chain above it that's just not part of the skeleton -- most
         * commonly a scene-root node carrying a corrective rotation
         * (a Z-up-authored rig's standard Z-up-to-Y-up fix on export,
         * exactly what game/assets/swat_operator/scene.gltf's own
         * "Sketchfab_model" root node does). armature_compute_world_
         * transforms treats parent==-1 as "this bone's own rest transform
         * IS its world transform" -- so silently using just the joint's
         * own local TRS here would drop that correction entirely, and
         * since the file's inverseBindMatrices were computed BY the
         * export tool assuming that correction IS present, the result
         * wouldn't even be self-consistent at rest pose (skin_matrix =
         * world*inverse_bind stops being identity) -- this is what made
         * an otherwise-correctly-loaded, correctly-scaled character
         * render lying on its back. Folding the ancestor chain's real
         * world transform (cgltf_node_transform_world already walks
         * every ancestor, not just the immediate parent) into this
         * joint's own rest transform keeps the rest of the pipeline
         * (armature_compute_world_transforms/_skinning_matrices, both
         * unmodified) working exactly as before -- this joint's "local"
         * transform now just happens to already include what used to be
         * missing. */
        if (b->parent == -1 && node->parent) {
            cgltf_float ancestor_world[16];
            cgltf_node_transform_world(node->parent, ancestor_world);
            float joint_local[16], composed[16];
            mat4_from_trs(b->rest_translation, b->rest_rotation, b->rest_scale, joint_local);
            phi_mat4_mul(composed, ancestor_world, joint_local);
            mat4_decompose_trs(composed, &b->rest_translation, &b->rest_rotation, &b->rest_scale);
        }

        /* cgltf_accessor_read_float, not raw buffer_view pointer math --
         * correctly handles a non-default stride/sparse accessor, same
         * established precedent halfedge_gltf.c's own accessor reads
         * already follow (cgltf_accessor_unpack_floats there). */
        if (!cgltf_accessor_read_float(ibm_acc, (cgltf_size)orig_i, b->inverse_bind, 16)) {
            printf("[armature] load failed: could not read inverseBindMatrices[%d]\n", orig_i);
            return 0;
        }
    }

    return 1;
}

int armature_find_bone(const Armature *arm, const char *name) {
    if (!arm || !name) return -1;
    for (int i = 0; i < arm->bone_count; i++)
        if (strcmp(arm->bones[i].name, name) == 0) return i;
    return -1;
}

void armature_rest_pose(const Armature *arm, Vec3f *out_t, Quat *out_r, Vec3f *out_s) {
    for (int i = 0; i < arm->bone_count; i++) {
        out_t[i] = arm->bones[i].rest_translation;
        out_r[i] = arm->bones[i].rest_rotation;
        out_s[i] = arm->bones[i].rest_scale;
    }
}

void armature_compute_world_transforms(const Armature *arm,
                                        const Vec3f *local_t, const Quat *local_r, const Vec3f *local_s,
                                        float out_world[][16]) {
    for (int i = 0; i < arm->bone_count; i++) {
        float local[16];
        mat4_from_trs(local_t[i], local_r[i], local_s[i], local);
        int parent = arm->bones[i].parent;
        if (parent < 0) {
            memcpy(out_world[i], local, 16 * sizeof(float));
        } else {
            /* parent < i always holds (parent-before-child, guaranteed by
             * armature_load_from_skin's topological sort), so
             * out_world[parent] is already valid here -- a single
             * forward pass, no recursion needed. */
            phi_mat4_mul(out_world[i], out_world[parent], local);
        }
    }
}

void armature_compute_skinning_matrices(const Armature *arm, const float world[][16], float out_skin[][16]) {
    for (int i = 0; i < arm->bone_count; i++)
        phi_mat4_mul(out_skin[i], world[i], arm->bones[i].inverse_bind);
}
