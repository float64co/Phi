#pragma once
#include "meshobject.h"
#include "phi_physics.h"
#include "fracture.h"
#include "renderer.h"

/* Runtime-activated Voronoi fracture fragments -- completes Phase 2's
 * "shatter on impact" gap (see phi.md's Phase 2 status): the fracture
 * tool (fracture.h) could already precompute fragments and the physics
 * layer (phi_physics.h) already had real convex-hull bodies and
 * breaking-threshold constraints, but nothing connected the two into a
 * live, simulated result -- this file is that connection.
 *
 * Each activated fragment is a real MeshObject (hem always NULL --
 * fragments aren't further mesh-editable, only simulated/rendered) with
 * its own convex-hull PhiRigidBody, so it renders through the EXISTING
 * renderer_draw_mesh_object completely unmodified. A separate, small,
 * fixed-capacity registry (PHI_MAX_FRACTURE_BODIES) -- same "bounded
 * registry, not a general scene graph" scope light.c's own Light
 * registry already established. Generalizing g_test_mesh_object itself
 * into a real array (so fragments could be picked/edited/listed in the
 * Outliner like a first-class object) would ripple into picking/
 * Outliner/Properties/every existing single-slot assumption throughout
 * main.c/ui.c -- a genuinely separate, much larger undertaking than
 * "shatter this mesh for real" actually requires. Fragments are
 * render+physics only: not selectable, not in the Outliner, not
 * Python-addressable by id. */

#define PHI_MAX_FRACTURE_BODIES 64

void fracture_body_system_init(void);

/* Activates a fracture: builds n_fragments Voronoi fragments from
 * source_hem (fracture_voronoi, already exists) in the source object's
 * own local space, spawns each non-empty one as a real MeshObject +
 * convex-hull PhiRigidBody at source_position/source_orientation (the
 * SAME transform the whole source object had -- each fragment's own
 * local-space vertex positions are what place it correctly within that,
 * so together the fragments reconstruct exactly what the source object
 * looked like at the moment of activation), computes which fragments
 * are adjacent (fracture_compute_adjacency, already exists) and glues
 * each adjacent pair with a real breaking-threshold fixed constraint
 * (phi_physics_add_fixed_constraint, already exists) at their shared
 * midpoint, so the shattered object still reads as one solid piece
 * until a real impact separates it -- Blender's own actual technique
 * for this (verified against blender/'s own rigidbody.cc this session,
 * see phi.md), not invented here.
 *
 * Clears any previously-active fragments first (fracture_body_clear) --
 * only one activation's worth of fragments exists at a time. Returns
 * the number of fragment bodies actually spawned (0 if source_hem is
 * NULL, fracture_voronoi itself fails, or every fragment came out
 * empty). */
int fracture_body_activate(PhiPhysicsWorld *world, const HalfEdgeMesh *source_hem,
                            Vec3f source_position, Quat source_orientation,
                            int n_fragments, unsigned int seed, float breaking_threshold);

/* Removes every live fragment body (and its constraints) from world and
 * frees them. Safe to call with nothing active (a no-op then). */
void fracture_body_clear(PhiPhysicsWorld *world);

int fracture_body_count(void);

/* Reads the index'th live fragment's current position/orientation
 * (whatever fracture_body_sync_and_draw_all last synced from its real
 * PhiRigidBody, or the spawn-time transform if that hasn't run yet this
 * frame) -- either out pointer may be NULL. Returns 0 (untouched) if
 * index is out of range ([0, fracture_body_count())). Mainly for
 * verification (see fracture_body_test_main.c), but a real, generally
 * useful accessor, not a test-only hook. */
int fracture_body_get(int index, Vec3f *out_position, Quat *out_orientation);

/* Applies a real impulse to the index'th live fragment's own physics
 * body (wraps phi_physics_apply_impulse) -- a no-op if index is out of
 * range. Kept as a dedicated wrapper rather than exposing each
 * fragment's raw PhiRigidBody* to callers, matching this file's own
 * "fragments aren't first-class Python-addressable objects" scope
 * note; mainly for verification (fracture_body_test_main.c drives a
 * real gentle-vs-hard-impulse break/no-break check through this, same
 * technique phi_physics_test.c already established for the underlying
 * constraint primitive), but a real capability either way. */
void fracture_body_apply_impulse(int index, Vec3f impulse, Vec3f rel_pos);

/* Syncs each fragment MeshObject's position/orientation from its own
 * PhiRigidBody (same "sync FROM the simulated body every frame" pattern
 * main.c's main_loop already uses for g_test_mesh_object) then draws
 * it via the existing renderer_draw_mesh_object -- call once per frame,
 * after phi_physics_world_step, from the Scene panel's draw_scene_content
 * callback. */
void fracture_body_sync_and_draw_all(Renderer *r);
