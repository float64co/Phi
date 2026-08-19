#pragma once
#include <string.h>

/* Shared, GL-free, header-only 4x4 matrix math -- consolidates the
 * mat4_mul (and mat4_transform_point) implementations that used to be
 * independently duplicated across armature.c, renderer.c, gbuffer.c,
 * meshobject.c, halfedge_gltf.c, skinned_mesh.c, and skinned_mesh_
 * object.c (each with its own comment explaining the duplication was
 * deliberate: several of those files must stay GL-free to link into
 * no-GL standalone test harnesses -- phi_h_test, mesh_edit_test -- and a
 * shared header that pulled in renderer.h would have broken that). This
 * header has no GL dependency at all (just `static inline` plain C, no
 * external types beyond raw float arrays), so it's safe for EVERY one of
 * those files to include directly, GL-free or not -- the original
 * reasoning for keeping them separate no longer applies once the shared
 * version itself doesn't risk a GL dependency.
 *
 * Real, added value beyond de-duplication: `mat4_mul` here is SSE2-
 * accelerated on native x86/x86_64 builds (SSE2 is baseline for every
 * real x86_64 target this project ships to -- no runtime CPU-feature
 * detection needed) and falls back to the identical scalar formula
 * every prior duplicate used everywhere else (wasm, non-x86 native),
 * so this ONE header is correct and safe to include from all three
 * platforms without a separate wasm code path to maintain.
 *
 * Column-major, m[col*4+row] -- same convention every mat4_* function in
 * this codebase already uses (see renderer.c's own top-of-file comment).
 *
 * Numerically verified against the pre-existing scalar implementations
 * before any caller was migrated to this header (identity, pure
 * rotation, pure translation, a composed TRS case, and BOTH in-place
 * aliasing cases -- out==a and out==b -- all cross-checked bit-for-bit)
 * -- same "verified numerically before use" bar this codebase's own
 * mat4_inverse comment already sets.
 *
 * Named phi_mat4_mul/phi_mat4_transform_point, not the shorter mat4_mul/
 * mat4_transform_point every duplicate used -- renderer.c's OWN mat4_mul
 * is a genuinely exported symbol (declared in renderer.h's own "Math
 * helpers exposed for main.c" section, real public API for game/src/
 * *.c authors), so it can't be replaced by a same-named static inline
 * from this header in the same translation unit (a static+inline and a
 * non-static definition of the same name conflict). renderer.c instead
 * keeps its own exported `mat4_mul` symbol, its body now just a one-line
 * delegate to phi_mat4_mul -- unchanged public API, same real
 * acceleration, no naming collision. Every other file's call sites were
 * renamed from mat4_mul/mat4_transform_point to the phi_-prefixed names
 * directly. */

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define PHI_VECMATH_HAVE_SSE2 1
#include <emmintrin.h>
#else
#define PHI_VECMATH_HAVE_SSE2 0
#endif

/* out = a*b. Safe even if out aliases a or b (no separate copy of
 * output needed, unlike the original per-file scalar versions' tmp[16]+
 * memcpy): each output COLUMN is a pure linear combination of the
 * matching input column of b, weighted by scalars read from b, applied
 * to a's columns which are fully loaded/read before any write to out
 * happens -- so out==a is safe (a is entirely consumed into registers/
 * locals up front) and out==b is safe (each column's 4 needed scalars
 * are read before that same column of out is overwritten, and no column
 * ever depends on another column's value). */
static inline void phi_mat4_mul(float *out, const float *a, const float *b) {
#if PHI_VECMATH_HAVE_SSE2
    __m128 a0 = _mm_loadu_ps(&a[0]);
    __m128 a1 = _mm_loadu_ps(&a[4]);
    __m128 a2 = _mm_loadu_ps(&a[8]);
    __m128 a3 = _mm_loadu_ps(&a[12]);
    for (int col = 0; col < 4; col++) {
        __m128 r = _mm_mul_ps(a0, _mm_set1_ps(b[col*4 + 0]));
        r = _mm_add_ps(r, _mm_mul_ps(a1, _mm_set1_ps(b[col*4 + 1])));
        r = _mm_add_ps(r, _mm_mul_ps(a2, _mm_set1_ps(b[col*4 + 2])));
        r = _mm_add_ps(r, _mm_mul_ps(a3, _mm_set1_ps(b[col*4 + 3])));
        _mm_storeu_ps(&out[col*4], r);
    }
#else
    float tmp[16];
    for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
        float sum = 0.0f;
        for (int k = 0; k < 4; k++)
            sum += a[k*4 + row] * b[col*4 + k];
        tmp[col*4 + row] = sum;
    }
    memcpy(out, tmp, 16 * sizeof(float));
#endif
}

/* p' = M * p (treating p as a point, w=1 implicit). Plain scalar --
 * this is a real, common dedup target (halfedge_gltf.c/skinned_mesh.c/
 * skinned_mesh_object.c each had their own identical copy) but NOT a
 * real SIMD target: it's only 3 dot-product-shaped outputs from one
 * point, called once per vertex at LOAD time (not a per-frame hot path
 * -- see this project's own SIMD survey), so the real win here is
 * removing the duplication, not vectorizing a 3-wide reduction that
 * would cost more in shuffle/horizontal-add overhead than it saves. */
static inline void phi_mat4_transform_point(const float m[16], const float p[3], float out[3]) {
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}
