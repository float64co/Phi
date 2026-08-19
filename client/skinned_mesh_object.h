#pragma once
#include "vec3.h"
#include "armature.h"
#include "animation.h"
#include "skinned_mesh.h"

/* Phase 4's GPU-skinning entity layer -- wraps skinned_mesh.h's pure CPU
 * data (SkinnedMesh/Armature/AnimClip, deliberately GL-free, see their
 * own header comments) with a real transform, playback state, and the
 * per-frame CPU-side pose->world->skin-matrix pipeline every GPU vertex
 * skinning implementation needs (the GPU only ever applies already-
 * computed matrices per vertex; computing THOSE matrices, once per frame
 * from the current animation pose, is real CPU work, not something the
 * shader can do itself) -- same "entity struct owns transform + a GPU
 * handle, computed data lives on it too" shape MeshObject already has
 * (RenderMesh::vbo/dirty), just for the skinned-mesh case. Still no
 * direct GL calls HERE (glGenBuffers/glBufferData/draw all live in
 * renderer.c's renderer_draw_skinned_mesh, matching the existing
 * meshobject.c/renderer.c split) -- vbo/ebo are just opaque GLuint
 * handles this struct carries so the renderer knows what to reuse
 * between frames instead of re-uploading every draw. */

typedef struct {
    SkinnedMesh mesh;
    Armature    arm;
    AnimClip    clips[8];        /* small fixed cap -- this phase's test/character content has at most a handful of clips, not a real asset-library scale */
    int         clip_count;
    AnimPlayback playback;

    Vec3f position;
    Quat  orientation;
    Vec3f scale;

    /* Fixed per-object material (SkinnedVertex carries no per-face
     * material data, unlike MeshObject's HEFace -- a real, honest scope
     * limit this pass: one uniform PBR material per skinned object, not
     * wired into the Properties panel yet either). Same neutral default
     * halfedge.h's own new-face default uses (0.7 gray, non-metallic,
     * roughness 0.8, no emission). */
    Vec3f base_color;
    float metallic;
    float roughness;
    Vec3f emission;

    /* Per-frame pipeline output (armature_compute_world_transforms then
     * armature_compute_skinning_matrices, see armature.h) -- recomputed
     * every skinned_mesh_object_update call, read by renderer_draw_
     * skinned_mesh as the u_bones[] uniform array upload. */
    float world[ARMATURE_MAX_BONES][16];
    float skin[ARMATURE_MAX_BONES][16];

    /* Opaque GL handles + upload-needed flag -- glGenBuffers'd once by
     * renderer_draw_skinned_mesh on first draw, left alone afterward
     * (mesh.verts/indices never change after load, unlike MeshObject's
     * render_mesh which can be rebuilt by editing ops -- so unlike
     * RenderMesh::dirty this flag only ever needs to go 1->0 once, never
     * back to 1, for this phase's scope: no runtime skinned-mesh editing
     * exists). 0 = not yet uploaded. */
    unsigned int vbo, ebo;
    int gpu_uploaded;
} SkinnedMeshObject;

/* Loads mesh+armature+every animation clip from path (see skinned_mesh_
 * load_gltf/animation_load_clips), starts the first loaded clip playing
 * (looped) if there is one, and seeds the rest pose otherwise. Returns 0
 * on failure (bad path, missing skin/JOINTS_0/WEIGHTS_0 -- see skinned_
 * mesh_load_gltf's own contract), 1 on success. *out is zeroed on
 * failure, safe to pass to skinned_mesh_object_free either way. */
int skinned_mesh_object_load(const char *path, Vec3f position, SkinnedMeshObject *out);

/* Advances obj->playback by dt, samples the current clip (or holds rest
 * pose if nothing's playing/no clips exist), and recomputes world[]/
 * skin[] -- call once per frame before drawing. Pure CPU, no GL. */
void skinned_mesh_object_update(SkinnedMeshObject *obj, float dt);

/* Frees mesh/clips allocations (does NOT delete GL buffers -- that's
 * renderer.c's job, since this file never touches GL; main.c's own
 * teardown calls both). Safe on an already-freed or zeroed object. */
void skinned_mesh_object_free(SkinnedMeshObject *obj);

/* Local-space AABB (min/max corners) of obj's CURRENT skinned pose (obj->
 * skin, already valid right after skinned_mesh_object_load -- see its
 * own comment on seeding rest pose) -- deliberately NOT a raw scan of
 * obj->mesh.verts' own bind-pose positions (meshobject_local_aabb_half_
 * extents' similarly-shaped counterpart for a HalfEdgeMesh IS a raw scan,
 * correct there since a static mesh has no skin matrix in play at all).
 * A skinned character's bind-pose data can sit in a completely different
 * orientation than its actual on-screen pose whenever a topmost joint's
 * real ancestor chain carried a correction armature_load_from_skin now
 * folds into that joint's rest transform (see its own comment) -- e.g. a
 * Z-up-authored rig's standard export-time Z-up-to-Y-up fix. Scanning
 * raw bind-pose Y in that case measures the character's PRE-correction
 * depth, not its real height, silently producing a wildly wrong scale
 * factor (and a wrong ground-contact Y) for anything derived from it --
 * exactly what made an otherwise-correctly-oriented character render
 * huge even after the armature fix corrected its orientation. This
 * applies the SAME per-vertex weighted skin-matrix blend renderer_draw_
 * skinned_mesh's own vertex shader does (see SKINNED_VERT_SRC_FMT), just
 * on the CPU, so the measured bounds match what actually ends up on
 * screen -- full min/max (not just half-extents) so a caller can derive
 * BOTH a height-based scale factor (max.y-min.y) and a ground-contact Y
 * offset (min.y) from one properly-skinned scan, instead of needing a
 * second pass. Returns 0 (out untouched) if obj->mesh has no vertices. */
int skinned_mesh_object_local_aabb(const SkinnedMeshObject *obj, Vec3f *out_min, Vec3f *out_max);
