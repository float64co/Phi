#include "gizmo.h"
#include <math.h>
#include <string.h>

#define GIZMO_LEN       24.0f  /* world-space arrow length */
#define GIZMO_SHAFT_R    1.0f  /* shaft half-thickness on the two non-axis dims */
#define GIZMO_HANDLE_R   4.0f  /* half-size of the grab-handle cube at the tip */

typedef struct {
    int       dragging;
    GizmoAxis axis;
    Vec3f     drag_start_obj_pos;
    Vec3f     drag_plane_point;   /* ray-plane hit point at grab time, world space */
    Vec3f     drag_plane_normal;
} GizmoState;

static GizmoState g_gizmo = { 0, GIZMO_AXIS_NONE, {0,0,0}, {0,0,0}, {0,0,0} };

/* ---- Vector helpers (file-static, matching physics.c/meshobject.c's own
 * vec3_* naming convention rather than a shared header — see meshobject.c's
 * comment on why duplicating these per translation unit beats a new
 * "vec3 utils" header for a handful of one-line functions). ---- */
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

static const Vec3f AXIS_DIR[3]      = { {1,0,0}, {0,1,0}, {0,0,1} };
static const float AXIS_COLOR[3][3] = { {0.9f,0.2f,0.2f}, {0.2f,0.85f,0.2f}, {0.25f,0.45f,0.95f} };

static void axis_handle_bounds(Vec3f obj_pos, int axis, Vec3f *tip, Vec3f *bmin, Vec3f *bmax) {
    *tip = vec3_add(obj_pos, vec3_scale(AXIS_DIR[axis], GIZMO_LEN));
    bmin->x = tip->x - GIZMO_HANDLE_R; bmax->x = tip->x + GIZMO_HANDLE_R;
    bmin->y = tip->y - GIZMO_HANDLE_R; bmax->y = tip->y + GIZMO_HANDLE_R;
    bmin->z = tip->z - GIZMO_HANDLE_R; bmax->z = tip->z + GIZMO_HANDLE_R;
}

void gizmo_draw(Renderer *r, Vec3f obj_pos) {
    for (int i = 0; i < 3; i++) {
        Vec3f tip = vec3_add(obj_pos, vec3_scale(AXIS_DIR[i], GIZMO_LEN));
        /* Shaft: a thin box spanning obj_pos->tip, padded by GIZMO_SHAFT_R
         * on the two axes it doesn't run along, so it renders as a long
         * thin wireframe "arrow" regardless of which axis it is. */
        Vec3f smin = {
            fminf(obj_pos.x, tip.x) - (i == 0 ? 0.0f : GIZMO_SHAFT_R),
            fminf(obj_pos.y, tip.y) - (i == 1 ? 0.0f : GIZMO_SHAFT_R),
            fminf(obj_pos.z, tip.z) - (i == 2 ? 0.0f : GIZMO_SHAFT_R),
        };
        Vec3f smax = {
            fmaxf(obj_pos.x, tip.x) + (i == 0 ? 0.0f : GIZMO_SHAFT_R),
            fmaxf(obj_pos.y, tip.y) + (i == 1 ? 0.0f : GIZMO_SHAFT_R),
            fmaxf(obj_pos.z, tip.z) + (i == 2 ? 0.0f : GIZMO_SHAFT_R),
        };
        /* Solid, not wireframe -- renderer_draw_wire_box's 1-pixel GL_LINES
         * edges turned out to vanish in the final TAA/FXAA-resolved frame
         * even though the geometry pass genuinely rasterized them (see
         * renderer_draw_solid_box's own comment for the full diagnosis). */
        renderer_draw_solid_box(r, smin, smax, AXIS_COLOR[i][0], AXIS_COLOR[i][1], AXIS_COLOR[i][2]);

        Vec3f hmin, hmax, htip;
        axis_handle_bounds(obj_pos, i, &htip, &hmin, &hmax);
        renderer_draw_solid_box(r, hmin, hmax, AXIS_COLOR[i][0], AXIS_COLOR[i][1], AXIS_COLOR[i][2]);
    }
}

static int ray_vs_aabb(Vec3f ro, Vec3f rd, Vec3f bmin, Vec3f bmax, float *out_t) {
    float tmin = 0.0f, tmax = 1e30f;
    float ro_c[3]   = { ro.x, ro.y, ro.z };
    float rd_c[3]   = { rd.x, rd.y, rd.z };
    float bmin_c[3] = { bmin.x, bmin.y, bmin.z };
    float bmax_c[3] = { bmax.x, bmax.y, bmax.z };
    for (int i = 0; i < 3; i++) {
        if (fabsf(rd_c[i]) < 1e-8f) {
            if (ro_c[i] < bmin_c[i] || ro_c[i] > bmax_c[i]) return 0;
            continue;
        }
        float inv = 1.0f / rd_c[i];
        float t0 = (bmin_c[i] - ro_c[i]) * inv;
        float t1 = (bmax_c[i] - ro_c[i]) * inv;
        if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmin > tmax) return 0;
    }
    *out_t = tmin;
    return 1;
}

GizmoAxis gizmo_pick_handle(Vec3f obj_pos, Vec3f ray_origin, Vec3f ray_dir) {
    GizmoAxis best_axis = GIZMO_AXIS_NONE;
    float best_t = 1e30f;
    for (int i = 0; i < 3; i++) {
        Vec3f tip, bmin, bmax;
        axis_handle_bounds(obj_pos, i, &tip, &bmin, &bmax);
        float t;
        if (ray_vs_aabb(ray_origin, ray_dir, bmin, bmax, &t) && t < best_t) {
            best_t = t;
            best_axis = (GizmoAxis)i;
        }
    }
    return best_axis;
}

void gizmo_begin_drag(GizmoAxis axis, Vec3f obj_pos, Vec3f ray_origin, Vec3f ray_dir) {
    g_gizmo.dragging = 1;
    g_gizmo.axis = axis;
    g_gizmo.drag_start_obj_pos = obj_pos;

    Vec3f axis_dir = AXIS_DIR[axis];
    /* Drag plane contains the axis and faces the camera (standard
     * technique: normal = axis x (view x axis), i.e. the plane spanned by
     * the axis and the camera-to-object direction) — dragging along the
     * screen-space projection of the axis then reads as smooth, stable
     * 1D motion regardless of viewing angle, rather than a plane that
     * happens to be perpendicular to the axis (which degenerates when
     * looking straight along it). */
    Vec3f view_dir = vec3_norm(vec3_sub(obj_pos, ray_origin));
    Vec3f plane_normal = vec3_cross(axis_dir, vec3_cross(view_dir, axis_dir));
    if (vec3_len(plane_normal) < 1e-4f) {
        /* Axis nearly parallel to the view direction -- the cross-product
         * construction degenerates (looking straight down the handle).
         * Fall back to a plane perpendicular to a world axis that isn't
         * the drag axis, so dragging still works from this angle. */
        Vec3f fallback = (axis == GIZMO_AXIS_Y) ? (Vec3f){1,0,0} : (Vec3f){0,1,0};
        plane_normal = vec3_cross(axis_dir, fallback);
    }
    plane_normal = vec3_norm(plane_normal);
    g_gizmo.drag_plane_normal = plane_normal;

    float denom = vec3_dot(plane_normal, ray_dir);
    if (fabsf(denom) > 1e-6f) {
        float t = vec3_dot(vec3_sub(obj_pos, ray_origin), plane_normal) / denom;
        g_gizmo.drag_plane_point = vec3_add(ray_origin, vec3_scale(ray_dir, t));
    } else {
        g_gizmo.drag_plane_point = obj_pos;
    }
}

int gizmo_update_drag(Vec3f *obj_pos, Vec3f ray_origin, Vec3f ray_dir) {
    if (!g_gizmo.dragging) return 0;
    float denom = vec3_dot(g_gizmo.drag_plane_normal, ray_dir);
    if (fabsf(denom) > 1e-6f) {
        float t = vec3_dot(vec3_sub(g_gizmo.drag_plane_point, ray_origin), g_gizmo.drag_plane_normal) / denom;
        Vec3f hit = vec3_add(ray_origin, vec3_scale(ray_dir, t));
        Vec3f delta = vec3_sub(hit, g_gizmo.drag_plane_point);
        float along = vec3_dot(delta, AXIS_DIR[g_gizmo.axis]);
        *obj_pos = vec3_add(g_gizmo.drag_start_obj_pos, vec3_scale(AXIS_DIR[g_gizmo.axis], along));
    }
    return 1;
}

void gizmo_end_drag(void) {
    g_gizmo.dragging = 0;
    g_gizmo.axis = GIZMO_AXIS_NONE;
}

int gizmo_is_dragging(void) { return g_gizmo.dragging; }
