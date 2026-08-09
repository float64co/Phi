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
