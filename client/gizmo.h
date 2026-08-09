#pragma once
#include "vec3.h"     /* Vec3f */
#include "renderer.h"

/* Phase 1 transform gizmo — translate only for this first pass (rotate/
 * scale are stubbed out, see phi.md: shipping one solid mode beats three
 * half-working ones). Three axis handles (X=red, Y=green, Z=blue) drawn
 * at the selected object's world position, reusing renderer_draw_wire_box
 * (already-verified rendering code, no new shader) for both the shaft
 * (a thin elongated box along the axis) and the grab handle (a small cube
 * at the tip) — a deliberately simple first-pass visual, not Blender-grade
 * cone-and-shaft geometry. */

typedef enum { GIZMO_AXIS_NONE = -1, GIZMO_AXIS_X = 0, GIZMO_AXIS_Y, GIZMO_AXIS_Z } GizmoAxis;

/* Draws the gizmo at obj_pos — call once per frame from the Scene panel's
 * content callback whenever something is selected. */
void gizmo_draw(Renderer *r, Vec3f obj_pos);

/* Ray-vs-handle hit test (real ray/AABB, not a 2D screen-space guess) —
 * returns which axis handle the ray hits nearest, or GIZMO_AXIS_NONE.
 * Checked before falling through to normal object-body picking so
 * grabbing a handle takes priority over re-picking the object under it. */
GizmoAxis gizmo_pick_handle(Vec3f obj_pos, Vec3f ray_origin, Vec3f ray_dir);

/* Begins a drag on the given axis — call on LMB press when
 * gizmo_pick_handle() returns a real axis. Sets up a drag plane
 * containing the axis and oriented toward the ray's origin (the camera),
 * the standard technique for stable single-axis dragging from 2D mouse
 * input (Blender/Unity's simple gizmos use the same idea). */
void gizmo_begin_drag(GizmoAxis axis, Vec3f obj_pos, Vec3f ray_origin, Vec3f ray_dir);

/* Call every frame a drag is active (LMB still held) with the CURRENT
 * mouse ray — moves *obj_pos in place along the grabbed axis. Returns 0
 * (and leaves *obj_pos untouched) if no drag is active, 1 otherwise. */
int gizmo_update_drag(Vec3f *obj_pos, Vec3f ray_origin, Vec3f ray_dir);

void gizmo_end_drag(void);
int  gizmo_is_dragging(void);
