#include "transform_op.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Vector/quaternion helpers (file-static, matching gizmo.c's/
 * meshobject.c's own vec3_* naming convention rather than a shared
 * header -- see gizmo.c's comment on why duplicating these per
 * translation unit beats a new "vec3 utils" header for a handful of
 * one-line functions). quat_mul/quat_from_axis_angle are new here --
 * verified against meshobject.h's quat_to_mat4 convention (a 90-degree
 * case composed with itself gives the expected 180-degree result) before
 * use, matching this codebase's own bar for quaternion/matrix code. ---- */
static inline Vec3f vec3_add(Vec3f a, Vec3f b) { return (Vec3f){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3f vec3_sub(Vec3f a, Vec3f b) { return (Vec3f){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3f vec3_scale(Vec3f a, float s) { return (Vec3f){a.x*s, a.y*s, a.z*s}; }
static inline float vec3_dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3f vec3_cross(Vec3f a, Vec3f b) {
    return (Vec3f){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float vec3_len(Vec3f v) { return sqrtf(vec3_dot(v, v)); }
static inline Vec3f vec3_norm(Vec3f v) {
    float l = vec3_len(v);
    if (l < 1e-6f) return (Vec3f){0,0,0};
    return vec3_scale(v, 1.0f / l);
}

/* Standard Hamilton product, a*b -- composes as "apply b first, then a"
 * (R(a*b)v == R(a)(R(b)v)), the same convention meshobject.h's
 * quat_to_mat4 rotation matrix already implements. */
static Quat quat_mul(Quat a, Quat b) {
    return (Quat){
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}
static Quat quat_from_axis_angle(Vec3f axis, float angle_rad) {
    float half = angle_rad * 0.5f;
    float s = sinf(half);
    return (Quat){ axis.x*s, axis.y*s, axis.z*s, cosf(half) };
}

static const Vec3f AXIS_DIR[3] = { {1,0,0}, {0,1,0}, {0,0,1} };

typedef struct {
    int active;
    TransformOpKind kind;
    TransformAxis   axis;

    /* Snapshot taken at transform_op_begin -- untouched by axis-lock
     * changes, restored verbatim by transform_op_cancel. */
    Vec3f orig_position;
    Quat  orig_orientation;
    Vec3f orig_scale;

    /* Baseline for the CURRENT axis-lock segment -- reset every time the
     * lock changes (including NONE<->X/Y/Z), so switching axes mid-drag
     * continues smoothly from the object's current live transform
     * instead of jumping back to the very start of the whole operation.
     * Same "recompute from segment start, never accumulate incremental
     * per-frame deltas" philosophy main.c's cam_orbit_from_start/
     * cam_pan_from_start already established for camera dragging. */
    Vec3f seg_position;     /* Grab */
    Vec3f seg_scale;        /* Scale */
    Quat  seg_orientation;  /* Rotate -- orientation this segment started from */
    int   seg_mouse_x, seg_mouse_y;

    /* Grab, axis-locked segments only -- drag-plane anchor, the exact
     * same technique gizmo.c's gizmo_begin_drag/gizmo_update_drag uses
     * for stable single-axis dragging from 2D mouse input, reimplemented
     * here (rather than calling into gizmo.c) so this modal op owns its
     * own independent lifecycle instead of sharing gizmo.c's
     * module-level dragging state, which is specifically about
     * handle-click drags and would conflict with this one. */
    Vec3f plane_point, plane_normal;

    /* Rotate only -- typed-degrees numeric override (see
     * transform_op.h's own comment: digit keys accumulate here and, once
     * non-empty, replace the mouse-driven angle entirely until cleared
     * back to empty via Backspace). */
    char typed_digits[16];
    int  typed_len;
} XformState;

static XformState g_xf = {0};

int             transform_op_active(void) { return g_xf.active; }
TransformOpKind transform_op_kind(void)   { return g_xf.kind; }
TransformAxis   transform_op_axis(void)   { return g_xf.axis; }

void transform_op_begin(TransformOpKind kind, Vec3f *position, Quat *orientation, Vec3f *scale,
                         const TransformCamCtx *cam, int mouse_x, int mouse_y) {
    (void)cam;  /* free-mode segments need no plane setup at begin time */
    memset(&g_xf, 0, sizeof(g_xf));
    g_xf.active = 1;
    g_xf.kind = kind;
    g_xf.axis = XFORM_AXIS_NONE;
    g_xf.orig_position    = *position;
    g_xf.orig_orientation = orientation ? *orientation : quat_identity();
    g_xf.orig_scale       = scale ? *scale : (Vec3f){1,1,1};
    g_xf.seg_position     = *position;
    g_xf.seg_scale        = g_xf.orig_scale;
    g_xf.seg_orientation  = g_xf.orig_orientation;
    g_xf.seg_mouse_x = mouse_x;
    g_xf.seg_mouse_y = mouse_y;
}

void transform_op_cancel(Vec3f *position, Quat *orientation, Vec3f *scale) {
    if (!g_xf.active) return;
    *position = g_xf.orig_position;
    if (orientation) *orientation = g_xf.orig_orientation;
    if (scale)        *scale       = g_xf.orig_scale;
    g_xf.active = 0;
    g_xf.kind = XFORM_NONE;
}

void transform_op_confirm(void) {
    g_xf.active = 0;
    g_xf.kind = XFORM_NONE;
}

/* Sets up the drag plane for a freshly-locked Grab axis, from the
 * object's position at the START of this lock segment (g_xf.seg_position)
 * -- see gizmo.c's gizmo_begin_drag for the identical technique and its
 * own fuller explanation of the plane construction. */
static void begin_axis_segment_grab(const TransformCamCtx *cam) {
    Vec3f axis_dir = AXIS_DIR[g_xf.axis];
    Vec3f view_dir = vec3_norm(vec3_sub(g_xf.seg_position, cam->ray_origin));
    Vec3f plane_normal = vec3_cross(axis_dir, vec3_cross(view_dir, axis_dir));
    if (vec3_len(plane_normal) < 1e-4f) {
        Vec3f fallback = (g_xf.axis == XFORM_AXIS_Y) ? (Vec3f){1,0,0} : (Vec3f){0,1,0};
        plane_normal = vec3_cross(axis_dir, fallback);
    }
    plane_normal = vec3_norm(plane_normal);
    g_xf.plane_normal = plane_normal;

    float denom = vec3_dot(plane_normal, cam->ray_dir);
    if (fabsf(denom) > 1e-6f) {
        float t = vec3_dot(vec3_sub(g_xf.seg_position, cam->ray_origin), plane_normal) / denom;
        g_xf.plane_point = vec3_add(cam->ray_origin, vec3_scale(cam->ray_dir, t));
    } else {
        g_xf.plane_point = g_xf.seg_position;
    }
}

static void update_grab(Vec3f *position, const TransformCamCtx *cam, int mouse_x, int mouse_y) {
    if (g_xf.axis == XFORM_AXIS_NONE) {
        /* Free move: screen-space mouse delta projected onto the
         * camera's own right/up plane, same technique main.c's
         * cam_pan_from_start uses for panning the camera -- here it
         * moves the OBJECT with the cursor directly (no sign flip on
         * the right term the way pan needs, since pan moves the
         * opposite thing: the camera, not the world). */
        float sc = cam->distance * 0.0025f;
        int dx = mouse_x - g_xf.seg_mouse_x;
        int dy = mouse_y - g_xf.seg_mouse_y;
        Vec3f delta = vec3_add(vec3_scale(cam->right, (float)dx * sc),
                                vec3_scale(cam->up, -(float)dy * sc));
        *position = vec3_add(g_xf.seg_position, delta);
        return;
    }
    float denom = vec3_dot(g_xf.plane_normal, cam->ray_dir);
    if (fabsf(denom) < 1e-6f) return;
    float t = vec3_dot(vec3_sub(g_xf.plane_point, cam->ray_origin), g_xf.plane_normal) / denom;
    Vec3f hit = vec3_add(cam->ray_origin, vec3_scale(cam->ray_dir, t));
    Vec3f delta = vec3_sub(hit, g_xf.plane_point);
    float along = vec3_dot(delta, AXIS_DIR[g_xf.axis]);
    *position = vec3_add(g_xf.seg_position, vec3_scale(AXIS_DIR[g_xf.axis], along));
}

static void update_scale(Vec3f *scale, int mouse_x) {
    /* Exponential-in-pixels multiplicative factor -- doubles every ~100px
     * dragged right, halves every ~100px dragged left, so scale can never
     * cross zero/go negative regardless of drag distance (a linear
     * pixels-to-factor mapping would). Simpler than Blender's own
     * screen-space-distance-from-object-center ratio, a deliberate
     * simplification matching this codebase's established "real but not
     * Blender-grade polish" bar (see main.c's fixed-distance extrude). */
    if (!scale) return;   /* defensive -- callers must never pass XFORM_SCALE for a NULL-scale target, but a stray call shouldn't crash */
    float factor = powf(2.0f, (float)(mouse_x - g_xf.seg_mouse_x) * 0.01f);
    if (g_xf.axis == XFORM_AXIS_NONE) {
        *scale = vec3_scale(g_xf.seg_scale, factor);
        return;
    }
    *scale = g_xf.seg_scale;
    float *comp = (g_xf.axis == XFORM_AXIS_X) ? &scale->x
                : (g_xf.axis == XFORM_AXIS_Y) ? &scale->y
                :                               &scale->z;
    *comp *= factor;
}

static void update_rotate(Quat *orientation, const TransformCamCtx *cam, int mouse_x) {
    /* No axis lock: Blender's own default is a trackball rotation around
     * the view axis -- approximated here as a straight rotation around
     * the camera's forward vector driven by horizontal mouse delta,
     * simpler than a true screen-space-angle-around-center computation
     * but directionally correct and predictable (same simplification
     * class as update_scale's factor curve above). */
    if (!orientation) return;   /* defensive, same reasoning as update_scale's guard */
    Vec3f world_axis = (g_xf.axis != XFORM_AXIS_NONE) ? AXIS_DIR[g_xf.axis] : cam->fwd;
    float angle;
    if (g_xf.typed_len > 0) {
        angle = (float)atof(g_xf.typed_digits) * (float)M_PI / 180.0f;
    } else {
        const float SENS_ROT = 0.01f;  /* radians per pixel of drag */
        angle = (float)(mouse_x - g_xf.seg_mouse_x) * SENS_ROT;
    }
    Quat delta = quat_from_axis_angle(world_axis, angle);
    *orientation = quat_normalize(quat_mul(delta, g_xf.seg_orientation));
}

void transform_op_update(Vec3f *position, Quat *orientation, Vec3f *scale, InputState *inp,
                          const TransformCamCtx *cam, int mouse_x, int mouse_y) {
    if (!g_xf.active) return;

    /* Modal ops own all keyboard input while active -- drain the shared
     * typed-char queue completely (see transform_op.h's own comment) so
     * nothing leaks through to the console/chat text fields underneath,
     * even for characters this function doesn't itself recognize. */
    char chars[TYPED_CHAR_QUEUE_SIZE];
    int nchars = inp->typed_count;
    memcpy(chars, inp->typed_chars, (size_t)nchars);
    inp->typed_count = 0;
    int backspace = inp->backspace_edge;
    inp->backspace_edge = 0;

    TransformAxis new_axis = g_xf.axis;
    int axis_changed = 0;
    for (int i = 0; i < nchars; i++) {
        char c = chars[i];
        TransformAxis pressed = XFORM_AXIS_NONE;
        if (c == 'x' || c == 'X') pressed = XFORM_AXIS_X;
        else if (c == 'y' || c == 'Y') pressed = XFORM_AXIS_Y;
        else if (c == 'z' || c == 'Z') pressed = XFORM_AXIS_Z;
        if (pressed != XFORM_AXIS_NONE) {
            /* Pressing the already-locked axis again unlocks it --
             * Blender's own convention for the same X/Y/Z keys. */
            new_axis = (new_axis == pressed) ? XFORM_AXIS_NONE : pressed;
            axis_changed = 1;
            continue;
        }
        if (g_xf.kind == XFORM_ROTATE && c >= '0' && c <= '9' &&
            g_xf.typed_len < (int)sizeof(g_xf.typed_digits) - 1) {
            g_xf.typed_digits[g_xf.typed_len++] = c;
            g_xf.typed_digits[g_xf.typed_len] = 0;
        }
    }
    if (g_xf.kind == XFORM_ROTATE && backspace && g_xf.typed_len > 0) {
        g_xf.typed_digits[--g_xf.typed_len] = 0;
    }

    if (axis_changed) {
        g_xf.axis = new_axis;
        g_xf.seg_position    = *position;
        g_xf.seg_scale       = scale ? *scale : g_xf.seg_scale;
        g_xf.seg_orientation = orientation ? *orientation : g_xf.seg_orientation;
        g_xf.seg_mouse_x = mouse_x;
        g_xf.seg_mouse_y = mouse_y;
        if (g_xf.kind == XFORM_GRAB && g_xf.axis != XFORM_AXIS_NONE) {
            begin_axis_segment_grab(cam);
        }
    }

    switch (g_xf.kind) {
        case XFORM_GRAB:   update_grab(position, cam, mouse_x, mouse_y); break;
        case XFORM_SCALE:  update_scale(scale, mouse_x); break;
        case XFORM_ROTATE: update_rotate(orientation, cam, mouse_x); break;
        default: break;
    }
}

void transform_op_hud_text(char *buf, int bufsz) {
    if (!g_xf.active || bufsz <= 0) { if (bufsz > 0) buf[0] = 0; return; }
    const char *kind_name = g_xf.kind == XFORM_GRAB ? "Grab"
                           : g_xf.kind == XFORM_SCALE ? "Scale"
                           : "Rotate";
    const char *axis_name = g_xf.axis == XFORM_AXIS_X ? " X"
                           : g_xf.axis == XFORM_AXIS_Y ? " Y"
                           : g_xf.axis == XFORM_AXIS_Z ? " Z"
                           : "";
    if (g_xf.kind == XFORM_ROTATE && g_xf.typed_len > 0) {
        snprintf(buf, (size_t)bufsz, "%s%s: %s (deg)", kind_name, axis_name, g_xf.typed_digits);
    } else {
        snprintf(buf, (size_t)bufsz, "%s%s", kind_name, axis_name);
    }
}
