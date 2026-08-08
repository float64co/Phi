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
