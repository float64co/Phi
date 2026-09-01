#pragma once

/* Minimal 3-vector types, used throughout the codebase (meshobject.h,
 * renderer.h, gizmo.h, mesh_edit.h, fracture.h, ...) independent of any
 * particular subsystem. Previously lived in octree.h (the Qek voxel
 * world's own header) purely as an accident of history -- these two
 * structs have nothing to do with octrees, so they get their own header
 * now that octree.h itself is gone (removed along with the rest of Qek's
 * gameplay/world code, see phi.md's Phase 1 status). */

typedef struct { float x, y, z; } Vec3f;
typedef struct { int   x, y, z; } Vec3i;

/* Axis-convention conversion (2026-08-19): Phi's own coordinate space is
 * Z-up (X=red, Y=green, Z=blue/vertical — Blender's own convention, per
 * explicit request); glTF's spec convention is Y-up. These two functions
 * are the fixed +90/-90 degree rotation about X that converts a position
 * between the two, used at the file boundary (halfedge_gltf.c's load/
 * save, armature.c's bone rest_translation, animation.c's translation
 * keyframes) so .gltf/.glb files on disk stay spec-compliant Y-up while
 * everything inside the engine — camera, physics, editor navigation — is
 * Z-up. This is exactly the axis handling Blender's own glTF importer/
 * exporter performs (Blender is Z-up internally too), not an invented
 * convention.
 *
 * The two are true inverses of each other but NOT the same operation
 * negated -- vec3_y_up_to_z_up applied twice does not return the input
 * (it's a 90-degree rotation, period 4, not a reflection); see each
 * function's own derivation. Old up (0,1,0) maps to new up (0,0,1) and
 * back, which is the one property actually worth eyeballing after any
 * change here. */
static inline Vec3f vec3_y_up_to_z_up(Vec3f v) { return (Vec3f){ v.x, -v.z, v.y }; }
static inline Vec3f vec3_z_up_to_y_up(Vec3f v) { return (Vec3f){ v.x, v.z, -v.y }; }

/* Non-uniform SCALE's own axis conversion -- deliberately NOT vec3_y_up_
 * to_z_up (a rotation, which negates one component; a per-axis scale
 * factor is a magnitude, not a signed position, so negating one would be
 * wrong -- it'd flip that axis's mirroring, not just relabel it).
 * Conjugating a diagonal scale matrix diag(sx,sy,sz) by the same fixed
 * +90-degree-about-X rotation (worked through by hand: R*diag(sx,sy,sz)*
 * R^-1 = diag(sx,sz,sy)) shows the right operation is a plain swap of
 * the y/z VALUES, no sign change -- self-inverse (apply it twice, get
 * the original back), unlike the position/rotation conversions above.
 * Used by armature.c's bone rest_scale and animation.c's scale
 * animation channels. */
static inline Vec3f vec3_swap_yz_for_scale(Vec3f s) { return (Vec3f){ s.x, s.z, s.y }; }
