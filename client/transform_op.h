#pragma once
#include "vec3.h"
#include "meshobject.h"
#include "input.h"

/* Blender-style modal G/S/R transform tool -- Grab/Scale/Rotate the
 * selected MeshObject with live mouse feedback, X/Y/Z axis locking,
 * Escape-to-cancel, and (Rotate only) numeric degree entry via the digit
 * keys. A self-contained mechanism module, same role gizmo.c already
 * plays for handle-click dragging -- this module owns none of the
 * decisions about WHEN to start/stop an operation or how its input
 * reaches here (see main.c's main_loop: hover-gated entry, confirm on
 * Enter/LMB, cancel on Escape/RMB, all main.c's own job), only the
 * mechanics of turning "modal op + mouse position + a few keys" into a
 * live position/orientation/scale on the object. */

typedef enum { XFORM_NONE = 0, XFORM_GRAB, XFORM_SCALE, XFORM_ROTATE } TransformOpKind;
typedef enum { XFORM_AXIS_NONE = -1, XFORM_AXIS_X = 0, XFORM_AXIS_Y, XFORM_AXIS_Z } TransformAxis;

/* Everything transform_op.c needs about the camera to turn mouse motion
 * into a world-space move/scale/rotate, bundled into one struct rather
 * than repeating five loose parameters across begin/update -- supplied by
 * main.c each frame (this module never reaches into g_renderer/g_cam_*
 * itself, same "explicit context in, no hidden globals" convention
 * ui.h's UIRenderContext already established). ray_origin/ray_dir are
 * the CURRENT mouse ray (main.c's compute_scene_ray) -- same technique
 * gizmo.c's axis-handle dragging already uses for stable single-axis
 * motion, reused here for axis-locked Grab rather than re-derived.
 * right/up/fwd are the camera's own basis (main.c's cam_basis) and
 * distance is g_cam_distance, both needed for the free-move/free-scale/
 * free-rotate (no axis lock) cases, which have no single axis to build a
 * drag plane from. */
typedef struct {
    Vec3f ray_origin, ray_dir;
    Vec3f right, up, fwd;
    float distance;
} TransformCamCtx;

int             transform_op_active(void);
TransformOpKind transform_op_kind(void);
TransformAxis   transform_op_axis(void);

/* Begins a modal operation on obj -- obj's CURRENT position/orientation/
 * scale become both the live working values (mutated in place every
 * subsequent transform_op_update call, so the Scene panel's own
 * rendering shows the transform live, same as gizmo dragging already
 * does) and the snapshot transform_op_cancel restores. Only one
 * operation can be active at a time -- calling this while already active
 * is a caller bug (main.c's own entry gating prevents it, see
 * transform_op_active()'s doc above). */
void transform_op_begin(TransformOpKind kind, MeshObject *obj, const TransformCamCtx *cam,
                         int mouse_x, int mouse_y);

/* Call once per frame while transform_op_active(), BEFORE any other
 * keyboard/mouse routing runs -- a modal op owns all relevant input
 * while active, matching Blender's own modal operators. Drains inp's
 * typed_chars/typed_count entirely (X/Y/Z toggles the axis lock --
 * pressing the currently-locked axis again unlocks it, Blender's own
 * convention; digit keys 0-9 accumulate a typed-degrees buffer, Rotate
 * only; anything else typed is silently discarded rather than left for
 * the console to pick up next frame) and inp->backspace_edge (edits the
 * typed-degrees buffer). Does NOT touch enter_edge/escape_edge/
 * lmb_click/rmb_click -- main.c reads and drains those itself to decide
 * confirm vs cancel, matching how every other one-shot InputState field
 * in this codebase is drained by its own single consumer. */
void transform_op_update(MeshObject *obj, InputState *inp, const TransformCamCtx *cam,
                          int mouse_x, int mouse_y);

/* Ends the op, restoring obj's position/orientation/scale to the
 * snapshot taken at transform_op_begin -- call on Escape/RMB-cancel. */
void transform_op_cancel(MeshObject *obj);

/* Ends the op, keeping obj's current (already-live) values -- call on
 * Enter/LMB-confirm. */
void transform_op_confirm(void);

/* Formats a short HUD readout of the in-progress operation into buf
 * (e.g. "Grab X", "Scale", "Rotate Z: 45 (deg)") -- writes an empty
 * string if no op is active. For main.c to hand to ui.c's Scene panel
 * via UIRenderContext, same "main.c formats, ui.c just draws" split
 * draw_panel_scene's mode label already uses. */
void transform_op_hud_text(char *buf, int bufsz);
