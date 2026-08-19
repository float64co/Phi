#pragma once
#include "skinned_mesh_object.h"

/* SkinnedMeshObject's own scene registry -- the same bounded-slot pattern
 * scene_objects.h already established for MeshObject (real, small, fixed-
 * capacity; addresses never move for a live object's whole lifetime, see
 * that file's own comment for the full rationale), kept as a SEPARATE
 * registry rather than widening scene_objects.h's MeshObject-typed API:
 * a skinned character is a genuinely different entity type (its own
 * armature/playback state, no per-face HEFace material, no phys_body),
 * same "different enough to earn its own thing rather than be squeezed
 * into the existing shape" reasoning meshobject.h's own MESHOBJ_VERTEX_
 * STRIDE comment gives for pbr_program existing alongside the shared one.
 *
 * Both editor_main.c's and player_main.c's own render loops iterate this
 * registry (alongside their existing scene_object_get_all/MeshObject
 * loop) so that ANY code that spawns a SkinnedMeshObject here -- game
 * code included -- gets it drawn and animated automatically, the same
 * "just add it to the registry" ergonomics MeshObject already has. Before
 * this registry existed, a skinned mesh could only ever be drawn by code
 * that manually called skinned_mesh_object_update/renderer_draw_skinned_
 * mesh itself every frame (fine for phi_h_test's one-off smoke test, not
 * a real multi-character game scene). */

#define SKINNED_SCENE_MAX_OBJECTS 8

void skinned_scene_objects_init(void);

/* Allocates a fresh slot: zeroed, a new stable id assigned (obj->id is
 * already set on return, same 4000+id-style convention scene_object_add
 * itself follows via SCENE_MAX_OBJECTS's own ids -- see renderer.h's
 * object-id-range comment; this registry uses its own separate counter,
 * so a skinned object's id is only unique WITHIN this registry, not
 * globally against MeshObject ids, same as how lights/fracture bodies
 * already each keep their own counter). Everything else left zero for
 * the caller to fill in via skinned_mesh_object_load. Returns NULL if the
 * registry is already at SKINNED_SCENE_MAX_OBJECTS. */
SkinnedMeshObject *skinned_scene_object_add(void);

/* Frees obj's mesh/clips allocations (skinned_mesh_object_free) and its
 * GL buffers if uploaded (glDeleteBuffers, mirroring mesh_destroy's own
 * vbo cleanup for MeshObject) -- returns the slot to the pool. Safe to
 * call with obj==NULL (a no-op). obj must not be used again after this
 * call. */
void skinned_scene_object_delete(SkinnedMeshObject *obj);

/* Fills out_objects (caller-owned, SKINNED_SCENE_MAX_OBJECTS capacity)
 * with pointers to every live object, returns the count -- same shape
 * scene_object_get_all already established. */
int skinned_scene_object_get_all(SkinnedMeshObject *out_objects[SKINNED_SCENE_MAX_OBJECTS]);

int skinned_scene_object_count(void);

/* id is this registry's own object id, NOT a shared/global one -- see
 * skinned_scene_object_add's own comment. */
int skinned_scene_object_id(const SkinnedMeshObject *obj);
