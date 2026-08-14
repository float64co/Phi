#include "light.h"
#include <math.h>
#include <string.h>

/* ---- Vector helpers (file-static, matching gizmo.c's/transform_op.c's
 * own vec3_* naming convention rather than a shared header -- same
 * "duplicating these per translation unit beats a new vec3-utils header"
 * reasoning gizmo.c's own comment already gives). Only the two
 * light_ray_pick's own sphere test needs -- vec3_add/scale/len/norm used
 * to also be needed by light_draw_all, which now lives in renderer.c
 * (see light.h's own comment on that split) and has its own copies. ---- */
static inline Vec3f vec3_sub(Vec3f a, Vec3f b) { return (Vec3f){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline float vec3_dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

typedef struct {
    PhiLight light;
    int      in_use;
} LightSlot;

static LightSlot s_lights[PHI_MAX_LIGHTS];
static int       s_next_id = 1;   /* monotonically increasing, never reused within a session -- matches MeshObject's own id convention (see meshobject.h), avoids a freed id being confused with a still-live one via a stale reference */

void light_system_init(void) {
    memset(s_lights, 0, sizeof(s_lights));
    s_next_id = 1;
}

/* Per-type defaults -- same rough order of magnitude Blender's own Light
 * DNA defaults use (energy=10 base, spotsize=45deg, spotblend=0.15,
 * area_size=0.25, sun_angle=0.526deg), scaled up for energy specifically
 * since this project's scene units run roughly 10-100x Blender's default
 * 1-2 unit scale (the test cube alone is 16 units) -- an arbitrary but
 * reasonable starting point given there's no path tracer yet to
 * calibrate real exposure against. */
static void apply_type_defaults(PhiLight *l) {
    l->color = (Vec3f){1.0f, 1.0f, 1.0f};
    l->direction = (Vec3f){0.0f, -1.0f, 0.0f};
    l->radius = 0.5f;
    l->spot_size = 0.7854f;   /* 45 degrees */
    l->spot_blend = 0.15f;
    l->area_size = 4.0f;
    l->sun_angle = 0.00918f;  /* Blender's own default, ~0.526 degrees */
    switch (l->type) {
        case LIGHT_TYPE_POINT: l->energy = 1000.0f; break;
        case LIGHT_TYPE_SUN:   l->energy = 5.0f;    break;
        case LIGHT_TYPE_SPOT:  l->energy = 1000.0f; break;
        case LIGHT_TYPE_AREA:  l->energy = 1000.0f; break;
        default:               l->energy = 1000.0f; break;
    }
}

PhiLight *light_spawn(LightType type, Vec3f position) {
    for (int i = 0; i < PHI_MAX_LIGHTS; i++) {
        if (s_lights[i].in_use) continue;
        LightSlot *slot = &s_lights[i];
        memset(&slot->light, 0, sizeof(PhiLight));
        slot->light.id = s_next_id++;
        slot->light.type = type;
        slot->light.position = position;
        apply_type_defaults(&slot->light);
        slot->in_use = 1;
        return &slot->light;
    }
    return NULL;
}

int light_delete(int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < PHI_MAX_LIGHTS; i++) {
        if (s_lights[i].in_use && s_lights[i].light.id == id) {
            s_lights[i].in_use = 0;
            return 1;
        }
    }
    return 0;
}

PhiLight *light_find(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < PHI_MAX_LIGHTS; i++) {
        if (s_lights[i].in_use && s_lights[i].light.id == id) return &s_lights[i].light;
    }
    return NULL;
}

int light_get_all(PhiLight *out_lights[PHI_MAX_LIGHTS]) {
    int n = 0;
    for (int i = 0; i < PHI_MAX_LIGHTS; i++) {
        if (s_lights[i].in_use) out_lights[n++] = &s_lights[i].light;
    }
    return n;
}

static int ray_vs_sphere(Vec3f ro, Vec3f rd, Vec3f center, float radius, float *out_t) {
    Vec3f oc = vec3_sub(ro, center);
    float a = vec3_dot(rd, rd);
    if (a < 1e-12f) return 0;
    float b = 2.0f * vec3_dot(oc, rd);
    float c = vec3_dot(oc, oc) - radius * radius;
    float disc = b*b - 4.0f*a*c;
    if (disc < 0.0f) return 0;
    float sqrt_disc = sqrtf(disc);
    float t0 = (-b - sqrt_disc) / (2.0f*a);
    float t1 = (-b + sqrt_disc) / (2.0f*a);
    float t = t0 >= 0.0f ? t0 : t1;
    if (t < 0.0f) return 0;
    *out_t = t;
    return 1;
}

PhiLight *light_ray_pick(Vec3f ray_origin, Vec3f ray_dir, float *out_t) {
    PhiLight *best = NULL;
    float best_t = 1e30f;
    for (int i = 0; i < PHI_MAX_LIGHTS; i++) {
        if (!s_lights[i].in_use) continue;
        float t;
        if (ray_vs_sphere(ray_origin, ray_dir, s_lights[i].light.position, LIGHT_ICON_RADIUS, &t) && t < best_t) {
            best_t = t;
            best = &s_lights[i].light;
        }
    }
    if (best && out_t) *out_t = best_t;
    return best;
}

