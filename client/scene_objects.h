#pragma once
#include "meshobject.h"
#include "phi_physics.h"

/* Real multi-object scene graph -- replaces the single g_test_mesh_object
 * slot main.c carried since Phase 1 (see its own retired comment: "There's
 * still only ever one test-object slot... this spawns/reloads that one
 * slot, it doesn't add a new independent object to a list, since no such
 * list exists yet"). That list now exists. Same bounded-registry pattern
 * light.c/fracture_body.c already established for exactly this reason
 * (real, small, fixed-capacity -- not a general dynamic scene graph with
 * unbounded growth): a fixed array of slots, addresses NEVER move for a
 * live object's whole lifetime (no realloc, ever), so every other system
 * that holds a `MeshObject *` across frames (Properties/Python/scene_
 * target's "object" resolution, the gizmo, transform_op, a Python-side
 * object-id round trip) can keep doing so safely.
 *
 * IDs are monotonically increasing and never reused (same convention
 * fracture_body.c's s_next_id/light.c's own id counter already use), so
 * a stale id from a since-deleted object can never silently collide with
 * a new one. Selection continues to use the existing `4000+id` range
 * (see ui.h's own comment on the convention) -- unchanged, since it was
 * always "id", not "the one slot's id" specifically. */

#define SCENE_MAX_OBJECTS 32

void scene_objects_init(void);

/* Allocates a fresh slot: zeroed, a new stable id assigned (obj->id is
 * already set on return), everything else left zero for the caller to
 * fill in (position/orientation/scale/hem/render_mesh/is_static/
 * phys_body -- same "allocate the slot, caller populates it" division as
 * fracture_body.c's own fragment spawning). Returns NULL if the registry
 * is already at SCENE_MAX_OBJECTS. The returned pointer is stable for
 * this object's entire lifetime (until scene_object_delete). */
MeshObject *scene_object_add(void);

/* Frees obj's render_mesh/hem (mesh_destroy/halfedge_destroy) and, if
 * obj->phys_body is set, removes it from phys_world too (phys_world may
 * be NULL only if the caller already knows obj has no physics body --
 * passing NULL with a live phys_body is a caller bug, same ownership
 * discipline phi_physics_remove_body itself already requires). Returns
 * the slot to the pool -- obj must not be used again after this call
 * (same convention as free()). Safe to call with obj==NULL (a no-op). */
void scene_object_delete(MeshObject *obj, PhiPhysicsWorld *phys_world);

/* Finds the live object with this id, NULL if none (already deleted or
 * never existed) -- same shape light_find/fracture_body's own lookups use. */
MeshObject *scene_object_find(int id);

/* Fills out_objects (caller-owned, SCENE_MAX_OBJECTS capacity) with
 * pointers to every live object, returns the count -- same shape
 * light_get_all already established, callable from any file (ui.c's
 * Outliner, main.c's drawing/picking/render-tracer collection) without
 * needing the pointer threaded through UIRenderContext -- lights already
 * work this same way. */
int scene_object_get_all(MeshObject *out_objects[SCENE_MAX_OBJECTS]);

int scene_object_count(void);
