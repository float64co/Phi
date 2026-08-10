#pragma once
#include "vec3.h"
#include "armature.h"
#include <stdint.h>

/* Phase 4 (see phi.md's "Animation Editor" -- "Armatures and skinning"):
 * the vertex format + loading path GPU vertex skinning will consume.
 * Deliberately still no GL/shader/render code here -- see phi.md's
 * status note on why that's sequenced after this, not written blind. */

#define SKINNED_MESH_MAX_INFLUENCE 4

typedef struct {
    float   pos[3];
    float   normal[3];
    uint8_t bone_idx[SKINNED_MESH_MAX_INFLUENCE];   /* indices into the Armature loaded alongside this mesh */
    float   bone_wgt[SKINNED_MESH_MAX_INFLUENCE];   /* NOT renormalized -- see skinned_mesh_load_gltf's own comment */
} SkinnedVertex;

typedef struct {
    SkinnedVertex *verts;         /* heap-allocated, owned by this struct */
    int            vert_count;
    uint16_t      *indices;       /* heap-allocated, owned by this struct -- triangle list, 3 per triangle */
    int            index_count;
} SkinnedMesh;

/* Loads a skinned mesh + its armature from a glTF file's first skin and
 * first mesh primitive -- self-contained (parses the file itself, same
 * "just a path in" convention as halfedge_load_gltf), not a wrapper
 * around an already-open cgltf_data. Requires the primitive to carry
 * POSITION, NORMAL, JOINTS_0, and WEIGHTS_0 attributes and be a triangle
 * list (mode 4) -- returns 0 (out params untouched) if any are missing,
 * the primitive isn't triangles, or armature loading itself fails. A
 * real character export always has all four attributes; this isn't a
 * fallback path for non-skinned meshes (halfedge_gltf.c already covers
 * that case).
 *
 * JOINTS_0 REMAPPING (the one subtle correctness point here): a JOINTS_0
 * value in the glTF file is an index into skin.joints, in that skin's
 * OWN (possibly arbitrary) order -- but armature_load_from_skin
 * topologically re-sorts bones into parent-before-child order, so a raw
 * JOINTS_0 value does NOT directly address the resulting Armature's
 * bones array. Each joint index is remapped via the ORIGINAL joint
 * node's name (skin->joints[raw_index]->name) looked up in the already-
 * sorted Armature (armature_find_bone) -- correct regardless of
 * whatever the file's own joints-array order happened to be, same
 * "don't trust the file's ordering" discipline armature_load_from_skin
 * itself already established.
 *
 * Weights are copied through AS-IS, NOT renormalized even if they don't
 * sum to 1.0 in the source file -- garbage in, garbage out, flagged
 * rather than silently "corrected" in a way that could mask a real
 * authoring bug upstream. */
int skinned_mesh_load_gltf(const char *path, Armature *out_arm, SkinnedMesh *out_mesh);

/* Frees out_mesh's verts/indices allocations (safe on an already-freed
 * or zeroed mesh). Does not free out_mesh itself, same convention as
 * animation_clip_free/halfedge_destroy. */
void skinned_mesh_free(SkinnedMesh *mesh);
