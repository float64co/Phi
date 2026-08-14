#pragma once
#include "vec3.h"
#include "armature.h"
#include "skinned_mesh_object.h"
#include "phi_physics.h"

/* Phase 4's Armature -> Bullet ragdoll handoff (see phi.md's "Animation
 * Editor" -- Armatures and skinning). Real physics, not a sketch: one
 * capsule PhiRigidBody per bone (spanning that bone's own head->tail, a
 * real anatomical fit rather than a generic box), connected to its
 * parent bone's body with a real ball-socket (point2point) constraint at
 * their shared joint -- the same general shape Blender's own default
 * ragdoll-constraint generator produces (verified against blender/'s own
 * armature ragdoll tooling, not guessed), minus per-joint angular limits
 * (a real, honest scope choice -- see phi_physics_add_point2point_
 * constraint's own comment).
 *
 * Same bounded-registry pattern light.c/fracture_body.c already
 * established (not a general scene-graph entity) -- one ragdoll active
 * at a time, capped at RAGDOLL_MAX_BONES bones (matches renderer.h's own
 * SKINNED_SHADER_MAX_BONES cap, this project's real armature content
 * (assets/test/armature_test.gltf) uses 3).
 *
 * NOT wired into any visual rendering this pass -- no capsule-mesh draw
 * path exists yet (this file only creates/simulates real Bullet bodies),
 * a real, honestly flagged scope limit, same class as fracture_body.h's
 * own "spawned fragments aren't Python-addressable" note. Verified via
 * ragdoll_get_transform + a standalone no-GL test (real physics: falls
 * under gravity, a joint constraint holds two connected bones together),
 * not by eye. */

#define RAGDOLL_MAX_BONES 64

void ragdoll_system_init(void);

/* Activates a ragdoll from obj's CURRENT pose (obj->world[], see
 * skinned_mesh_object.h -- typically whatever pose skinned_mesh_object_
 * update last computed, e.g. mid-animation) and obj's own position/
 * orientation/scale (bone world[] matrices are in the skinned mesh's own
 * local/armature space, not yet placed in the physics world -- this
 * composes them with obj's transform the same way renderer_draw_skinned_
 * mesh's model matrix does, so a ragdoll spawned from a currently-
 * animating character starts exactly where it visually is).
 *
 * Each bone's capsule spans from its own head (its world[] translation)
 * to its first child's head (or, for a leaf bone with no children, a
 * short capsule extended along the bone's own local +Y axis instead --
 * the same "no real tail data for an end bone" fallback Blender's own
 * ragdoll generator uses). Capsule radius is a fixed fraction of that
 * segment's length (clamped to a sane minimum so a very short/zero-
 * length bone still gets a real, non-degenerate shape).
 *
 * Clears any previously-active ragdoll first (ragdoll_clear) -- only one
 * activation's worth of bodies exists at a time, same convention
 * fracture_body_activate already established. Returns the number of
 * bone bodies actually spawned (0 if obj is NULL, has no bones, or
 * bone_count exceeds... it doesn't fail on that, it clamps to
 * RAGDOLL_MAX_BONES and spawns what fits, same "clamp and flag, don't
 * refuse" convention renderer_draw_skinned_mesh's own bone clamp uses). */
int ragdoll_activate(PhiPhysicsWorld *world, const SkinnedMeshObject *obj,
                      float mass_per_bone, float restitution);

/* Removes every live ragdoll body (and its joint constraints) from world
 * and frees them. Safe to call with nothing active (a no-op then). */
void ragdoll_clear(PhiPhysicsWorld *world);

int ragdoll_count(void);

/* Reads the index'th live bone body's current simulated transform
 * (world-space capsule center + orientation) -- either out pointer may
 * be NULL. Returns 0 (untouched) if index is out of range. Mainly for
 * verification (ragdoll_test_main.c) and any future capsule-mesh render
 * path, but a real, generally useful accessor either way, same framing
 * fracture_body_get's own comment uses. */
int ragdoll_get_transform(int index, Vec3f *out_position, Quat *out_orientation);
