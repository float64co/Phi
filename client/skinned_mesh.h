#pragma once
#include "vec3.h"
#include "armature.h"
#include <stdint.h>

/* Phase 4 (see phi.md's "Animation Editor" -- "Armatures and skinning"):
 * the vertex format + loading path GPU vertex skinning consumes.
 *
 * Rewritten 2026-08-19 for real multi-part character imports (Sketchfab-
 * style exports: many meshes/primitives, many materials/textures, one
 * shared skin) -- see halfedge_gltf.c's own top comment for the static-
 * mesh twin of this same change and why it was needed (the original
 * loader only ever read mesh[0]/primitive[0]). */

#define SKINNED_MESH_MAX_INFLUENCE 4

typedef struct {
    float   pos[3];
    float   normal[3];
    float   uv[2];
    uint8_t bone_idx[SKINNED_MESH_MAX_INFLUENCE];   /* indices into the Armature loaded alongside this mesh */
    float   bone_wgt[SKINNED_MESH_MAX_INFLUENCE];   /* NOT renormalized -- see skinned_mesh_load_gltf's own comment */
} SkinnedVertex;

/* One real material/texture group within a merged multi-primitive
 * SkinnedMesh -- renderer_draw_skinned_mesh issues one glDrawElements
 * per submesh (see renderer.c), binding `texture` (0 = none, flat
 * base_color only) before each. index_start/index_count index into the
 * SAME shared SkinnedMesh::indices/verts buffers (one VBO/EBO for the
 * whole mesh, multiple draw ranges -- the standard real-time-engine
 * "submesh" pattern, not a separate GPU buffer per material). */
typedef struct {
    int          index_start;
    int          index_count;
    float        base_color[3];
    unsigned int texture;
} SkinnedSubmesh;

#define SKINNED_MESH_MAX_SUBMESHES 64

typedef struct {
    SkinnedVertex *verts;         /* heap-allocated, owned by this struct */
    int            vert_count;
    /* uint32_t, not uint16_t -- a real multi-primitive character merge
     * (see skinned_mesh_load_gltf's own comment) routinely exceeds 65536
     * shared vertices (swat_operator alone is ~103k), so a 16-bit index
     * would silently wrap and corrupt the mesh. renderer_draw_skinned_mesh
     * uploads/draws this with GL_UNSIGNED_INT accordingly. */
    uint32_t      *indices;       /* heap-allocated, owned by this struct -- triangle list, 3 per triangle */
    int            index_count;
    SkinnedSubmesh submeshes[SKINNED_MESH_MAX_SUBMESHES];
    int            submesh_count;
} SkinnedMesh;

/* Loads a skinned mesh + its armature from a glTF file's first skin,
 * merging EVERY mesh primitive reachable from the file's default scene
 * graph into one SkinnedMesh (see SkinnedSubmesh above for how per-
 * material/texture grouping survives the merge). Two real vertex shapes,
 * handled correctly rather than assumed away:
 *   - Primitives on a node bound to this skin (POSITION/NORMAL/JOINTS_0/
 *     WEIGHTS_0 all present): read directly in bind-pose/mesh space, no
 *     node-transform baking -- exactly the original single-primitive
 *     behavior, just looped across every such primitive now.
 *   - Primitives on a node with NO skin (a real, common case: rigid
 *     attachments like a helmet or weapon, parented under a joint node
 *     rather than smoothly skinned) -- these get RIGIDLY bound (weight
 *     1.0) to their nearest ancestor node that IS one of this skin's own
 *     joints, walking node->parent until one is found (falls back to
 *     bone 0 if none is -- a real, honest "attach to the root rather
 *     than drop the geometry" default, not a silent loss). Their vertex
 *     positions have that attachment node's own world transform baked
 *     in first (cgltf_node_transform_world), matching halfedge_gltf.c's
 *     own static-mesh transform-baking for exactly the same reason: an
 *     unskinned part's local vertex data isn't already in the shared
 *     mesh/bind space the skin's inverseBindMatrices assume, a skinned
 *     part's is.
 *
 * JOINTS_0 REMAPPING (the one subtle correctness point here, unchanged
 * from before this rewrite): a JOINTS_0 value in the glTF file is an
 * index into skin.joints, in that skin's OWN (possibly arbitrary) order
 * -- but armature_load_from_skin topologically re-sorts bones into
 * parent-before-child order, so a raw JOINTS_0 value does NOT directly
 * address the resulting Armature's bones array. Each joint index is
 * remapped via the ORIGINAL joint node's name (skin->joints[raw_index]->
 * name) looked up in the already-sorted Armature (armature_find_bone) --
 * correct regardless of whatever the file's own joints-array order
 * happened to be, same "don't trust the file's ordering" discipline
 * armature_load_from_skin itself already established.
 *
 * Weights are copied through AS-IS, NOT renormalized even if they don't
 * sum to 1.0 in the source file -- garbage in, garbage out, flagged
 * rather than silently "corrected" in a way that could mask a real
 * authoring bug upstream.
 *
 * Returns 0 (out params untouched) if the file has no skin, or armature
 * loading itself fails. A real character export always has one; this
 * isn't a fallback path for non-skinned meshes (halfedge_gltf.c already
 * covers that case, including for a file with no skin at all). */
int skinned_mesh_load_gltf(const char *path, Armature *out_arm, SkinnedMesh *out_mesh);

/* Registers the real texture loader (texture_cache.c's texture_cache_
 * load) -- a function-pointer handoff, not a direct call/#include, for
 * the identical reason halfedge_gltf_register_texture_loader exists (see
 * its own comment): this file is linked into genuinely no-GL standalone
 * test harnesses (phi_h_test). Call once from a real engine executable's
 * startup; leave unregistered in a test harness, where every submesh's
 * texture just stays 0. */
void skinned_mesh_register_texture_loader(unsigned int (*load_texture)(const char *path));

/* Frees out_mesh's verts/indices allocations (safe on an already-freed
 * or zeroed mesh). Does not free out_mesh itself, same convention as
 * animation_clip_free/halfedge_destroy. */
void skinned_mesh_free(SkinnedMesh *mesh);

/* NOTE: there is deliberately no raw-bind-pose AABB helper here (an
 * earlier version of this file had one, since removed) -- a skinned
 * character's raw POSITION data can sit in a completely different
 * orientation than its actual on-screen pose (see armature_load_from_
 * skin's own comment on why), so scanning it directly gives a WRONG
 * answer for "how tall is this character" whenever that applies, not
 * just an approximate one. Use skinned_mesh_object_local_aabb (skinned_
 * mesh_object.h) instead -- it needs a SkinnedMeshObject, not just this
 * SkinnedMesh, because it applies the object's own current skin matrices
 * (armature_compute_skinning_matrices' output) per vertex, the same way
 * the GPU vertex shader does, before measuring. */
