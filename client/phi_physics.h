#pragma once
#include "vec3.h"
#include <stdint.h>

/* Phi's own thin C API over vendored Bullet Physics (client/vendor/
 * bullet3/, LinearMath+BulletCollision+BulletDynamics only -- see
 * phi.md's "Bullet Physics via Emscripten" section for what's vendored
 * and why). Bullet itself is C++; this header is plain C (guarded with
 * extern "C" below) so the rest of this codebase, which is C throughout,
 * never has to touch a Bullet header or C++ syntax directly -- same
 * "thin API over a real embedded subsystem" shape mp_port.h already
 * established for MicroPython.
 *
 * Correction to phi.md's original text: Bullet's core rigid-body engine
 * (BulletDynamics/BulletCollision) does NOT ship an official C API --
 * the "*_C_API" files upstream belong to PyBullet's separate shared-
 * memory RPC layer, a different subsystem entirely, confirmed by reading
 * the actual vendored source rather than assuming the original doc text
 * was accurate (same verify-before-assuming discipline this project's
 * MicroPython CRITICAL WARNING section already established). This file
 * is a real, hand-written wrapper, not upstream's own C surface.
 *
 * Units: gravity defaults to real-world (0,-9.81,0), i.e. this codebase's
 * scene units are assumed roughly meter-scale for Bullet's solver
 * tolerances to behave sanely -- not verified against the actual scene
 * scale (the one test object spans ~16 units), flagged as an open
 * question rather than silently assumed correct. */

typedef struct PhiPhysicsWorld PhiPhysicsWorld;   /* opaque */
typedef struct PhiRigidBody    PhiRigidBody;      /* opaque */
typedef struct PhiConstraint   PhiConstraint;     /* opaque */

#ifdef __cplusplus
extern "C" {
#endif

PhiPhysicsWorld *phi_physics_world_create(void);
void phi_physics_world_destroy(PhiPhysicsWorld *world);
void phi_physics_world_set_gravity(PhiPhysicsWorld *world, Vec3f gravity);

/* Advances the simulation by `dt` seconds, internally subdividing into
 * up to 10 fixed 1/60s substeps (Bullet's own standard recommended usage
 * for a variable-dt caller) -- callers don't need their own fixed-
 * timestep accumulator, this already provides one. */
void phi_physics_world_step(PhiPhysicsWorld *world, float dt);

/* mass == 0.0f creates a static/immovable body (Bullet's own convention
 * for "infinite mass") -- used for a ground plane/collider that objects
 * rest on but nothing ever moves. orientation is (x,y,z,w), matching
 * meshobject.h's own Quat layout exactly (this header deliberately
 * doesn't redefine its own quaternion type to avoid a second, easy-to-
 * mismatch representation existing side by side). */
PhiRigidBody *phi_physics_add_box_body(PhiPhysicsWorld *world, Vec3f half_extents,
                                        Vec3f position, float orientation[4],
                                        float mass, float restitution);

/* Builds a genuine reduced convex hull from an arbitrary point cloud
 * (positions: tightly-packed xyz float triples, vert_count of them) via
 * Bullet's own real quickhull implementation (btConvexHullComputer,
 * already vendored) -- the same technique Blender's own rigid-body
 * system uses for its default "Convex Hull" collision shape on dynamic
 * objects (verified by reading Blender's blenkernel/intern/rigidbody.cc
 * and intern/rigidbody/rb_bullet_api.cpp directly, not guessed),
 * including its own margin-embedding-with-fallback behavior: try to
 * shrink the hull inward by Bullet's usual 0.04 collision margin so the
 * margin is "embedded" in the shape rather than puffing it outward; if
 * that fails (a very thin/degenerate point cloud), fall back to an exact
 * zero-margin hull rather than refusing to create a shape at all. The
 * caller doesn't need to pre-reduce the point cloud -- btConvexHullComputer
 * does real hull reduction internally, so passing every vertex of a dense
 * mesh is fine, same as Blender's own call site does. */
PhiRigidBody *phi_physics_add_convex_hull_body(PhiPhysicsWorld *world,
                                                const float *positions, int vert_count,
                                                Vec3f position, float orientation[4],
                                                float mass, float restitution);

/* Removes `body` from `world` and frees it (and its collision shape/
 * motion state) -- safe to call once per phi_physics_add_*_body call,
 * not safe to call twice on the same pointer (same ownership convention
 * as free()). */
void phi_physics_remove_body(PhiPhysicsWorld *world, PhiRigidBody *body);

/* Reads the body's current simulated transform -- call once per frame,
 * after phi_physics_world_step, to sync a MeshObject's position/
 * orientation to wherever physics moved it. Either output pointer may be
 * NULL if not needed. orientation is written as (x,y,z,w). */
void phi_physics_get_transform(PhiRigidBody *body, Vec3f *out_position, float out_orientation[4]);

/* Directly overrides the body's transform (e.g. a gizmo-dragged position
 * for a kinematic/static object, or resetting after a test) -- wakes the
 * body if it was asleep. */
void phi_physics_set_transform(PhiRigidBody *body, Vec3f position, const float orientation[4]);

void phi_physics_apply_impulse(PhiRigidBody *body, Vec3f impulse, Vec3f rel_pos);
void phi_physics_set_linear_velocity(PhiRigidBody *body, Vec3f v);
Vec3f phi_physics_get_linear_velocity(PhiRigidBody *body);

/* ---- Breaking-threshold rigid constraints -- Bullet's own real
 * "glue two bodies together until enough force pulls them apart"
 * primitive, the same mechanism Blender's Rigid Body Constraint
 * ("Fixed" type + "Breaking" threshold) is built on (verified via
 * blenkernel/intern/rigidbody.cc and intern/rigidbody/rb_bullet_api.cpp:
 * RB_constraint_new_fixed wraps btFixedConstraint, RB_constraint_set_
 * breaking_threshold calls btTypedConstraint::setBreakingImpulseThreshold
 * -- exactly what this wraps). Bullet's own constraint solver checks the
 * applied impulse against the threshold every solved frame and disables
 * the constraint internally the moment it's exceeded (see vendored
 * btSequentialImpulseConstraintSolver.cpp) -- callers don't need to
 * poll/compute anything themselves beyond phi_physics_constraint_is_broken
 * below, which just reads Bullet's own flag.
 *
 * Intended use (see fracture.h's fracture_compute_adjacency): glue
 * adjacent Voronoi fracture fragments together at runtime with a real
 * breaking threshold, so a lightly-touched fractured object still reads
 * as one solid piece until an impact exceeds the threshold, at which
 * point exactly the fragments near the impact separate -- Blender's own
 * technique for "shatter on impact", not a different one invented here.
 * NOTE: wiring this all the way into a live on-screen demo needs a real
 * multi-object scene (spawning N fragment MeshObjects and removing the
 * original) which this engine doesn't have yet -- see phi.md's Phase 2
 * status for the honest scope line on what's blocked vs. what's real and
 * verified here. */
PhiConstraint *phi_physics_add_fixed_constraint(PhiPhysicsWorld *world,
                                                 PhiRigidBody *a, PhiRigidBody *b,
                                                 Vec3f pivot_world, float breaking_threshold);

/* Removes and frees the constraint -- same ownership convention as
 * phi_physics_remove_body (safe once, not safe twice). */
void phi_physics_remove_constraint(PhiPhysicsWorld *world, PhiConstraint *c);

/* 1 once Bullet's own solver has disabled this constraint for exceeding
 * its breaking threshold, 0 while still intact. */
int phi_physics_constraint_is_broken(const PhiConstraint *c);

#ifdef __cplusplus
}
#endif
