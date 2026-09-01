#pragma once
#include "vec3.h"
#include "meshobject.h"   /* Quat -- for aabb_world_bounds' orientation param */

/* Core-engine view-frustum culling -- GL-free (plain float/Vec3f math,
 * same "safe for no-GL standalone test harnesses" bar vecmath_simd.h's
 * own top comment sets), so it's usable from any draw-loop owner
 * (editor_main.c today; player_main.c/game code are free callers later)
 * without pulling in a GL dependency.
 *
 * Motivation: before this, every renderer_draw_* call for every live
 * scene object ran unconditionally every frame, with no visibility test
 * at all -- correct today at this project's current (tiny) test-scene
 * scale, but the first real cost multiplier once scene sizes grow, and
 * before any new rendering technique gets added on top (each of those
 * adds cost per object DRAWN, so cutting the draw set first is the
 * highest-leverage perf work available before anything else). */

typedef struct {
    /* planes[i] = (a,b,c,d) of ax+by+cz+d=0, normalized, normal pointing
     * INTO the frustum -- a point/box is inside iff every plane's signed
     * distance is >= 0. Order: left, right, bottom, top, near, far
     * (matches frustum_extract's own derivation order, not otherwise
     * significant to callers). */
    float planes[6][4];
} Frustum;

/* Extracts this frame's 6 view-frustum planes from a combined view-
 * projection matrix -- the standard Gribb/Hartmann method (row
 * combinations of the vp matrix), verified against the same column-major
 * m[col*4+row] convention every mat4_* function in this codebase uses
 * (renderer.c's own top-of-file comment) and OpenGL's [-1,1] NDC z range
 * (matching mat4_perspective's own m[10]/m[14], the only projection this
 * codebase builds). Pass renderer_get_view_proj's own output directly. */
void frustum_extract(const float *vp, Frustum *out);

/* Standard "positive vertex" AABB-vs-frustum test: for each plane, tests
 * only the box corner furthest along the plane's own normal; if even
 * that corner is on the outside, the whole box is outside. Conservative
 * in the caller's favor -- a box merely touching/straddling a plane
 * still counts as visible (a false-positive "still draw it" near the
 * frustum edge is harmless; a false-negative "wrongly culled" would be a
 * real, visible bug, so this never risks that side). Returns 1 (visible,
 * draw it) or 0 (fully outside, safe to skip). */
int frustum_intersects_aabb(const Frustum *f, Vec3f bmin, Vec3f bmax);

/* Transforms a local-space AABB (bmin/bmax, e.g. RenderMesh::local_bmin/
 * local_bmax) by a T*R*S transform (position/orientation/scale -- the
 * SAME model exactly, and in the same order, renderer_draw_mesh_object's
 * own T*R*S model matrix uses) into a real, tight world-space AABB --
 * Arvo's method (per-axis min/max of the linear part's scaled row
 * contributions), not a naive "transform all 8 corners" loop, though the
 * two are mathematically equivalent for an axis-aligned box under an
 * affine transform. Reuses quat_to_mat4 (meshobject.h) for the rotation
 * part rather than re-deriving quaternion-to-matrix math a second time. */
void aabb_world_bounds(Vec3f local_min, Vec3f local_max,
                        Vec3f position, Quat orientation, Vec3f scale,
                        Vec3f *out_min, Vec3f *out_max);
