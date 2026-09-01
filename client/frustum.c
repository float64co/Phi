#include "frustum.h"
#include <math.h>

void frustum_extract(const float *m, Frustum *out) {
    /* m is column-major (m[col*4+row]), so row i of the matrix is
     * (m[i], m[4+i], m[8+i], m[12+i]) -- read that way, not m[4*i+..],
     * which would silently transpose it. clip = m * v is inside OpenGL's
     * [-1,1] NDC cube iff -w <= x,y,z <= w component-wise; each pair of
     * inequalities below is exactly that, rearranged into a single plane
     * equation per bound (row3 +/- rowN >= 0). */
    float row0[4] = { m[0], m[4], m[8],  m[12] };
    float row1[4] = { m[1], m[5], m[9],  m[13] };
    float row2[4] = { m[2], m[6], m[10], m[14] };
    float row3[4] = { m[3], m[7], m[11], m[15] };

    for (int i = 0; i < 4; i++) out->planes[0][i] = row3[i] + row0[i]; /* left:   x + w >= 0 */
    for (int i = 0; i < 4; i++) out->planes[1][i] = row3[i] - row0[i]; /* right:  w - x >= 0 */
    for (int i = 0; i < 4; i++) out->planes[2][i] = row3[i] + row1[i]; /* bottom: y + w >= 0 */
    for (int i = 0; i < 4; i++) out->planes[3][i] = row3[i] - row1[i]; /* top:    w - y >= 0 */
    for (int i = 0; i < 4; i++) out->planes[4][i] = row3[i] + row2[i]; /* near:   z + w >= 0 */
    for (int i = 0; i < 4; i++) out->planes[5][i] = row3[i] - row2[i]; /* far:    w - z >= 0 */

    /* Normalize each plane's (a,b,c,d) by |(a,b,c)| -- not required for
     * frustum_intersects_aabb's own sign-only test, but keeps every
     * plane's `d` a real signed distance for any future caller (e.g. a
     * fade-near-the-edge effect) that wants one, at negligible per-frame
     * cost (6 planes). */
    for (int p = 0; p < 6; p++) {
        float *pl = out->planes[p];
        float len = sqrtf(pl[0]*pl[0] + pl[1]*pl[1] + pl[2]*pl[2]);
        if (len > 1e-8f) {
            float inv = 1.0f / len;
            pl[0] *= inv; pl[1] *= inv; pl[2] *= inv; pl[3] *= inv;
        }
    }
}

int frustum_intersects_aabb(const Frustum *f, Vec3f bmin, Vec3f bmax) {
    for (int p = 0; p < 6; p++) {
        const float *pl = f->planes[p];
        float px = pl[0] >= 0.0f ? bmax.x : bmin.x;
        float py = pl[1] >= 0.0f ? bmax.y : bmin.y;
        float pz = pl[2] >= 0.0f ? bmax.z : bmin.z;
        if (pl[0]*px + pl[1]*py + pl[2]*pz + pl[3] < 0.0f) return 0;
    }
    return 1;
}

void aabb_world_bounds(Vec3f local_min, Vec3f local_max,
                        Vec3f position, Quat orientation, Vec3f scale,
                        Vec3f *out_min, Vec3f *out_max) {
    /* rot's translation column is zero (quat_to_mat4 builds a pure
     * rotation) -- exactly the linear-part-only matrix Arvo's method
     * needs; T*R*S's own translation (obj->position) is added back in
     * directly below instead. */
    float rot[16];
    quat_to_mat4(&orientation, rot);

    float lmin[3] = { local_min.x, local_min.y, local_min.z };
    float lmax[3] = { local_max.x, local_max.y, local_max.z };
    float scl[3]  = { scale.x, scale.y, scale.z };
    float mn[3] = { position.x, position.y, position.z };
    float mx[3] = { position.x, position.y, position.z };

    /* For each world axis r, sum each local axis j's own contribution
     * (rotation-scaled) taken at whichever of local_min/local_max makes
     * it a min vs. a max for THIS output row -- the standard
     * transform-an-AABB-without-visiting-all-8-corners identity, exact
     * for an affine (R*S then translate) transform of an axis-aligned
     * box. */
    for (int j = 0; j < 3; j++) {
        for (int r = 0; r < 3; r++) {
            float e = rot[j*4 + r] * scl[j];
            float a = e * lmin[j];
            float b = e * lmax[j];
            mn[r] += a < b ? a : b;
            mx[r] += a < b ? b : a;
        }
    }

    out_min->x = mn[0]; out_min->y = mn[1]; out_min->z = mn[2];
    out_max->x = mx[0]; out_max->y = mx[1]; out_max->z = mx[2];
}
