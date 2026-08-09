#include "meshobject.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

Quat quat_identity(void) {
    Quat q = { 0.0f, 0.0f, 0.0f, 1.0f };
    return q;
}

void quat_to_mat4(const Quat *q, float *m) {
    float xx = q->x * q->x, yy = q->y * q->y, zz = q->z * q->z;
    float xy = q->x * q->y, xz = q->x * q->z, yz = q->y * q->z;
    float wx = q->w * q->x, wy = q->w * q->y, wz = q->w * q->z;

    memset(m, 0, 16 * sizeof(float));
    m[0]  = 1.0f - 2.0f * (yy + zz);
    m[1]  = 2.0f * (xy + wz);
    m[2]  = 2.0f * (xz - wy);
    m[4]  = 2.0f * (xy - wz);
    m[5]  = 1.0f - 2.0f * (xx + zz);
    m[6]  = 2.0f * (yz + wx);
    m[8]  = 2.0f * (xz + wy);
    m[9]  = 2.0f * (yz - wx);
    m[10] = 1.0f - 2.0f * (xx + yy);
    m[15] = 1.0f;
}

/* ---- Vector helpers, matching physics.c's own vec3_* naming convention
 * (file-static there too — no shared header for these, small enough that
 * duplicating per translation unit beats introducing a new "vec3 utils"
 * header just for this). vec3_cross doesn't exist in physics.c yet, added
 * here since Möller–Trumbore needs it. ---- */
static inline Vec3f vec3_add(Vec3f a, Vec3f b) { return (Vec3f){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3f vec3_sub(Vec3f a, Vec3f b) { return (Vec3f){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline float vec3_dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3f vec3_cross(Vec3f a, Vec3f b) {
    return (Vec3f){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

/* Rotates v by the rotation-only matrix quat_to_mat4 produces (translation
 * column is zero there — see its own comment) -- column-major, m[col*4+row],
 * same layout convention as renderer.c's mat4_* helpers. */
static Vec3f mat4_rotate_vec3(const float *m, Vec3f v) {
    return (Vec3f){
        m[0]*v.x + m[4]*v.y + m[8]*v.z,
        m[1]*v.x + m[5]*v.y + m[9]*v.z,
        m[2]*v.x + m[6]*v.y + m[10]*v.z
    };
}

/* Real Möller–Trumbore ray/triangle intersection — the standard technique
 * (1997), not invented here. Backface-culling-free (works from either
 * side, det can be negative) since picking should work regardless of
 * which way the clicked face happens to be wound. Returns 0 for a miss,
 * including "behind the ray origin" (t < EPS) and "parallel to the
 * triangle's plane" (|det| < EPS) cases. */
static int ray_intersect_triangle(Vec3f orig, Vec3f dir, Vec3f v0, Vec3f v1, Vec3f v2, float *out_t) {
    const float EPS = 1e-6f;
    Vec3f e1 = vec3_sub(v1, v0);
    Vec3f e2 = vec3_sub(v2, v0);
    Vec3f pvec = vec3_cross(dir, e2);
    float det = vec3_dot(e1, pvec);
    if (det > -EPS && det < EPS) return 0;
    float inv_det = 1.0f / det;
    Vec3f tvec = vec3_sub(orig, v0);
    float u = vec3_dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) return 0;
    Vec3f qvec = vec3_cross(tvec, e1);
    float v = vec3_dot(dir, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return 0;
    float t = vec3_dot(e2, qvec) * inv_det;
    if (t < EPS) return 0;
    *out_t = t;
    return 1;
}

/* Ray-vs-MeshObject picking: tests every triangle in render_mesh (already
 * flattened/vertex-duplicated-per-triangle, VERTEX_STRIDE=7 floats/vertex —
 * see octree_render.h), transformed into world space by the object's own
 * position+orientation, and returns the NEAREST hit (not just the first
 * triangle that happens to intersect) so overlapping/self-occluding
 * geometry picks the visually-correct face. O(triangle count) per call —
 * fine for this phase's small test assets (a handful to a few thousand
 * triangles); a real editor-scale scene with many objects would want a
 * bounding-volume broad phase per object before falling into this, not
 * yet needed with a single test-object slot (see g_test_mesh_object). */
int meshobject_ray_pick(const MeshObject *obj, Vec3f ray_origin, Vec3f ray_dir, float *out_t) {
    if (!obj->render_mesh || obj->render_mesh->count < 3) return 0;
    float rot[16];
    quat_to_mat4(&obj->orientation, rot);
    const float *data = obj->render_mesh->data;
    int tri_count = obj->render_mesh->count / 3;
    float best_t = -1.0f;
    for (int i = 0; i < tri_count; i++) {
        Vec3f local[3], world[3];
        for (int k = 0; k < 3; k++) {
            const float *vp = data + (size_t)(i * 3 + k) * VERTEX_STRIDE;
            local[k] = (Vec3f){ vp[0], vp[1], vp[2] };
            world[k] = vec3_add(mat4_rotate_vec3(rot, local[k]), obj->position);
        }
        float t;
        if (ray_intersect_triangle(ray_origin, ray_dir, world[0], world[1], world[2], &t)) {
            if (best_t < 0.0f || t < best_t) best_t = t;
        }
    }
    if (best_t < 0.0f) return 0;
    *out_t = best_t;
    return 1;
}

static void face_normal(const HalfEdgeMesh *hem, const int *verts, float *n) {
    const float *a = hem->verts[verts[0]].pos;
    const float *b = hem->verts[verts[1]].pos;
    const float *c = hem->verts[verts[2]].pos;
    float ux = b[0]-a[0], uy = b[1]-a[1], uz = b[2]-a[2];
    float vx = c[0]-a[0], vy = c[1]-a[1], vz = c[2]-a[2];
    n[0] = uy*vz - uz*vy;
    n[1] = uz*vx - ux*vz;
    n[2] = ux*vy - uy*vx;
    float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
}

void meshobject_build_render_mesh_from_halfedge(RenderMesh *out, const HalfEdgeMesh *hem, float mat_id) {
    out->count = 0;
    for (int f = 0; f < hem->face_count; f++) {
        int verts[3];
        halfedge_face_verts(hem, f, verts);
        float n[3];
        face_normal(hem, verts, n);
        for (int i = 0; i < 3; i++) {
            if (out->count >= out->capacity) {
                out->capacity *= 2;
                out->data = (float *)realloc(out->data, (size_t)out->capacity * VERTEX_STRIDE * sizeof(float));
            }
            const float *p = hem->verts[verts[i]].pos;
            float *v = out->data + out->count * VERTEX_STRIDE;
            v[0] = p[0]; v[1] = p[1]; v[2] = p[2];
            v[3] = n[0]; v[4] = n[1]; v[5] = n[2];
            v[6] = mat_id;
            out->count++;
        }
    }
    out->dirty = 1;
}
