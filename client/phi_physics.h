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
 * Scope for this first slice (see phi.md's Phase 2 status): only box
 * collision shapes (AABB half-extents), no convex hulls from arbitrary
 * meshes yet -- that's a real, separate algorithm (compute a convex hull
 * from a HalfEdgeMesh's vertices) deferred rather than half-built. Units:
 * gravity defaults to real-world (0,-9.81,0), i.e. this codebase's scene
 * units are assumed roughly meter-scale for Bullet's solver tolerances
 * to behave sanely -- not verified against the actual scene scale
 * (the one test object spans ~16 units), flagged as an open question
 * rather than silently assumed correct. */

typedef struct PhiPhysicsWorld PhiPhysicsWorld;   /* opaque */
typedef struct PhiRigidBody    PhiRigidBody;      /* opaque */

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

#ifdef __cplusplus
}
#endif
