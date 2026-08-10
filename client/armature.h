#pragma once
#include "vec3.h"
#include "meshobject.h"   /* Quat, quat_identity, quat_to_mat4 */
#include "cgltf.h"

/* Phase 4 (see phi.md's "Animation Editor" -- Armatures and skinning):
 * a real bone hierarchy loaded from a glTF skin, independent of any mesh
 * being bound to it (GPU vertex skinning against a real mesh's JOINTS_0/
 * WEIGHTS_0 attributes is later, separately-scoped work -- this module
 * is the Armature half of that, not the whole feature). */

#define ARMATURE_MAX_BONES 256
#define BONE_NAME_LEN       64

typedef struct {
    char  name[BONE_NAME_LEN];
    int   parent;              /* index into Armature.bones, -1 = root */
    Vec3f rest_translation;
    Quat  rest_rotation;
    Vec3f rest_scale;
    /* Column-major, straight from the skin's own inverseBindMatrices
     * accessor (glTF stores these per-joint, not derived) -- maps a
     * vertex from this bone's bind-pose world space back to mesh-local
     * space, the standard second half of the skinning matrix
     * (skin_matrix = bone_world * inverse_bind). */
    float inverse_bind[16];
} Bone;

typedef struct {
    Bone bones[ARMATURE_MAX_BONES];
    int  bone_count;
} Armature;

/* Loads an Armature from a cgltf_skin -- walks skin->joints (each a
 * cgltf_node*), extracting each joint's rest-pose TRS and its
 * inverseBindMatrices entry. Topologically sorts into PARENT-BEFORE-CHILD
 * order in the output array (glTF's own joints-array order is NOT
 * guaranteed to already be parent-before-child -- real authoring tools
 * can and do emit any order), so armature_compute_world_transforms below
 * can rely on "parent index < child index" and compute every bone's
 * world transform in one forward pass. A joint whose cgltf-level parent
 * node isn't itself one of this skin's joints becomes a second root
 * (parent = -1) rather than an error -- nothing here requires a single
 * root. Returns 0 on failure (NULL skin/out, too many joints for
 * ARMATURE_MAX_BONES, or a missing/malformed inverseBindMatrices
 * accessor), 1 on success. */
int armature_load_from_skin(const cgltf_skin *skin, Armature *out);

/* Finds a bone's index by name, -1 if not found. */
int armature_find_bone(const Armature *arm, const char *name);

/* Fills out_t/out_r/out_s (each arm->bone_count long) with the
 * armature's own rest pose -- the pose used when nothing is animating. */
void armature_rest_pose(const Armature *arm, Vec3f *out_t, Quat *out_r, Vec3f *out_s);

/* Computes every bone's WORLD-space matrix (column-major, same
 * convention as quat_to_mat4/renderer.c's mat4_*) from a per-bone LOCAL
 * pose (local_t/local_r/local_s, each arm->bone_count long -- typically
 * produced by animation_sample_clip, or just armature_rest_pose's own
 * output when nothing is animating). Relies on arm's bones already being
 * parent-before-child (guaranteed by armature_load_from_skin), computing
 * each bone's world matrix as parent_world * local_TRS in a single
 * forward pass -- no recursion needed. out_world must have
 * arm->bone_count entries. */
void armature_compute_world_transforms(const Armature *arm,
                                        const Vec3f *local_t, const Quat *local_r, const Vec3f *local_s,
                                        float out_world[][16]);

/* Computes every bone's SKINNING matrix (world[i] * inverse_bind[i]) from
 * already-computed world transforms (see armature_compute_world_
 * transforms) -- the standard second half of GPU vertex skinning: a
 * vertex bound to bone i with weight w is displaced by
 * w * (skin_matrix[i] * vertex_bind_pose_position). At rest pose (no
 * animation applied), every skin matrix is exactly identity by
 * construction (world[i] == the bone's own bind-pose world transform,
 * whose inverse is inverse_bind[i]) -- a real, checkable invariant, not
 * just "should be close to it". out_skin must have arm->bone_count
 * entries. Deliberately a plain data function, no GL/uniform-buffer
 * upload here -- that's the renderer's job once a real skinned-mesh
 * render path exists (not yet, see phi.md's Phase 4 status: this is
 * still the data-layer-only slice). */
void armature_compute_skinning_matrices(const Armature *arm, const float world[][16], float out_skin[][16]);
