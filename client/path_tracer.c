#include "path_tracer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- local vec3 helpers -- same "each file defines its own small
 * static-inline set rather than sharing a math header" convention this
 * project already established (see meshobject.c/fracture.c's own local
 * copies). ---- */
static inline Vec3f v3(float x, float y, float z) { return (Vec3f){x, y, z}; }
static inline Vec3f v3add(Vec3f a, Vec3f b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline Vec3f v3sub(Vec3f a, Vec3f b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline Vec3f v3mul(Vec3f a, Vec3f b) { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static inline Vec3f v3scale(Vec3f a, float s) { return v3(a.x*s, a.y*s, a.z*s); }
static inline float v3dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3f v3cross(Vec3f a, Vec3f b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline float v3len(Vec3f a) { return sqrtf(v3dot(a, a)); }
static inline Vec3f v3norm(Vec3f a) {
    float l = v3len(a);
    return (l > 1e-12f) ? v3scale(a, 1.0f / l) : v3(0.0f, 0.0f, 1.0f);
}
static inline Vec3f v3neg(Vec3f a) { return v3(-a.x, -a.y, -a.z); }
static inline float v3max3(Vec3f a) { float m = a.x > a.y ? a.x : a.y; return m > a.z ? m : a.z; }

/* Self-contained xorshift32, same technique/rationale as fracture.c's own
 * copy (portable to win32/mingw without relying on libc rand_r). */
static unsigned int xorshift32(unsigned int *state) {
    unsigned int x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x ? x : 1;
    return x;
}
static float rand01(unsigned int *state) {
    return (float)(xorshift32(state) & 0xFFFFFFu) / (float)0x1000000u;
}

/* Real Moller-Trumbore ray/triangle intersection, no backface culling
 * (works from either side -- a path tracer needs to shade whichever side
 * of a triangle the ray actually approaches from, same reasoning
 * meshobject.c's own copy of this documents for picking). Re-implemented
 * locally rather than shared, matching this project's per-file
 * convention for this exact routine. */
static int ray_intersect_triangle(Vec3f orig, Vec3f dir, Vec3f v0, Vec3f v1, Vec3f v2, float *out_t) {
    const float EPS = 1e-7f;
    Vec3f e1 = v3sub(v1, v0), e2 = v3sub(v2, v0);
    Vec3f pvec = v3cross(dir, e2);
    float det = v3dot(e1, pvec);
    if (det > -EPS && det < EPS) return 0;
    float inv_det = 1.0f / det;
    Vec3f tvec = v3sub(orig, v0);
    float u = v3dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) return 0;
    Vec3f qvec = v3cross(tvec, e1);
    float w = v3dot(dir, qvec) * inv_det;
    if (w < 0.0f || u + w > 1.0f) return 0;
    float t = v3dot(e2, qvec) * inv_det;
    if (t < EPS) return 0;
    *out_t = t;
    return 1;
}

/* Rotates v by the rotation-only matrix quat_to_mat4 produces --
 * identical formula to meshobject.c's own mat4_rotate_vec3 (column-major,
 * m[col*4+row]), re-derived locally per this file's own convention. */
static Vec3f mat4_rotate_vec3_local(const float *m, Vec3f v) {
    return v3(
        m[0]*v.x + m[4]*v.y + m[8]*v.z,
        m[1]*v.x + m[5]*v.y + m[9]*v.z,
        m[2]*v.x + m[6]*v.y + m[10]*v.z
    );
}

/* ============================== Scene build ============================== */

static void pt_scene_build_bvh(PTScene *scene);   /* defined below, in the BVH build section */

PTScene pt_scene_build(const MeshObject *const *objects, int n_objects) {
    PTScene scene = {0};
    int total_tris = 0;
    for (int i = 0; i < n_objects; i++) {
        if (objects[i] && objects[i]->render_mesh) total_tris += objects[i]->render_mesh->count / 3;
    }
    if (total_tris <= 0) return scene;

    scene.tris = (PTTriangle *)malloc(sizeof(PTTriangle) * (size_t)total_tris);
    scene.tri_count = 0;

    for (int oi = 0; oi < n_objects; oi++) {
        const MeshObject *obj = objects[oi];
        if (!obj || !obj->render_mesh) continue;
        float rot[16];
        quat_to_mat4(&obj->orientation, rot);
        const float *data = obj->render_mesh->data;
        int tri_count = obj->render_mesh->count / 3;
        for (int t = 0; t < tri_count; t++) {
            const float *va = data + (size_t)(t*3+0) * MESHOBJ_VERTEX_STRIDE;
            const float *vb = data + (size_t)(t*3+1) * MESHOBJ_VERTEX_STRIDE;
            const float *vc = data + (size_t)(t*3+2) * MESHOBJ_VERTEX_STRIDE;
            /* local * scale -> rotate -> + position, matching renderer.c's
             * own T*(R*S) model matrix exactly (see renderer_draw_mesh_
             * object) -- NOT the scale-ignoring transform meshobject_ray_
             * pick uses (that function's own scope note flags scale as
             * unhandled there; this tracer's job is to match what's
             * actually rendered, not that picking approximation). */
            Vec3f la = v3(va[0]*obj->scale.x, va[1]*obj->scale.y, va[2]*obj->scale.z);
            Vec3f lb = v3(vb[0]*obj->scale.x, vb[1]*obj->scale.y, vb[2]*obj->scale.z);
            Vec3f lc = v3(vc[0]*obj->scale.x, vc[1]*obj->scale.y, vc[2]*obj->scale.z);
            Vec3f wa = v3add(mat4_rotate_vec3_local(rot, la), obj->position);
            Vec3f wb = v3add(mat4_rotate_vec3_local(rot, lb), obj->position);
            Vec3f wc = v3add(mat4_rotate_vec3_local(rot, lc), obj->position);

            PTTriangle *tri = &scene.tris[scene.tri_count++];
            tri->v0 = wa; tri->v1 = wb; tri->v2 = wc;
            /* Recomputed from the actual world-space triangle rather than
             * transforming the stored per-vertex normal -- correct
             * regardless of non-uniform scale (a naive scale-then-rotate
             * of a stored normal is wrong under non-uniform scale without
             * an inverse-transpose; re-deriving from the already-correct
             * world positions sidesteps that entirely). */
            tri->n = v3norm(v3cross(v3sub(wb, wa), v3sub(wc, wa)));
            tri->base_color = v3(va[6], va[7], va[8]);
            tri->metallic   = va[9];
            tri->roughness  = va[10];
            tri->emission   = v3(va[11], va[12], va[13]);
        }
    }
    pt_scene_build_bvh(&scene);
    return scene;
}

void pt_scene_destroy(PTScene *scene) {
    if (!scene) return;
    free(scene->tris);
    free(scene->nodes);
    scene->tris = NULL; scene->nodes = NULL;
    scene->tri_count = 0; scene->node_count = 0;
}

/* ============================== BVH build ============================== */

static int g_bvh_sort_axis = 0;
static int bvh_tri_cmp(const void *pa, const void *pb) {
    const PTTriangle *a = (const PTTriangle *)pa, *b = (const PTTriangle *)pb;
    float ca, cb;
    if (g_bvh_sort_axis == 0)      { ca = a->v0.x + a->v1.x + a->v2.x; cb = b->v0.x + b->v1.x + b->v2.x; }
    else if (g_bvh_sort_axis == 1) { ca = a->v0.y + a->v1.y + a->v2.y; cb = b->v0.y + b->v1.y + b->v2.y; }
    else                            { ca = a->v0.z + a->v1.z + a->v2.z; cb = b->v0.z + b->v1.z + b->v2.z; }
    return (ca > cb) - (ca < cb);
}

/* Real median-split BVH, not a flat linear scan pretending to be one --
 * standard textbook technique (compute the node's own AABB, leaf below a
 * small triangle-count threshold, else split along the AABB's longest
 * axis at the median so both children get roughly equal triangle counts).
 * Not SAH (surface-area-heuristic) optimized -- a real, honest scope
 * choice for this pass's scene sizes (this engine currently has exactly
 * one live object slot plus its fracture fragments, at most a few
 * thousand triangles), not silently pretending median-split is SAH. */
static int bvh_build_recursive(PTTriangle *tris, int start, int count, PTBVHNode *nodes, int *node_count) {
    int idx = (*node_count)++;
    float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (int i = start; i < start + count; i++) {
        Vec3f vs[3] = { tris[i].v0, tris[i].v1, tris[i].v2 };
        for (int k = 0; k < 3; k++) {
            float p[3] = { vs[k].x, vs[k].y, vs[k].z };
            for (int a = 0; a < 3; a++) {
                if (p[a] < bmin[a]) bmin[a] = p[a];
                if (p[a] > bmax[a]) bmax[a] = p[a];
            }
        }
    }
    memcpy(nodes[idx].bmin, bmin, sizeof(bmin));
    memcpy(nodes[idx].bmax, bmax, sizeof(bmax));

    const int LEAF_MAX = 4;
    if (count <= LEAF_MAX) {
        nodes[idx].left = -1; nodes[idx].right = -1;
        nodes[idx].start = start; nodes[idx].count = count;
        return idx;
    }

    float ext[3] = { bmax[0]-bmin[0], bmax[1]-bmin[1], bmax[2]-bmin[2] };
    int axis = 0;
    if (ext[1] > ext[axis]) axis = 1;
    if (ext[2] > ext[axis]) axis = 2;
    g_bvh_sort_axis = axis;
    qsort(&tris[start], (size_t)count, sizeof(PTTriangle), bvh_tri_cmp);

    int mid = count / 2;
    int left  = bvh_build_recursive(tris, start, mid, nodes, node_count);
    int right = bvh_build_recursive(tris, start + mid, count - mid, nodes, node_count);
    /* nodes[] is a single fixed-size block (never reallocated mid-build,
     * see pt_scene_build_bvh below), so re-indexing here after the
     * recursive calls is safe -- no stale pointer risk. */
    nodes[idx].left = left; nodes[idx].right = right;
    nodes[idx].count = 0;
    return idx;
}

static void pt_scene_build_bvh(PTScene *scene) {
    if (scene->tri_count <= 0) { scene->nodes = NULL; scene->node_count = 0; return; }
    int max_nodes = 2 * scene->tri_count;   /* real upper bound for a binary tree over tri_count leaves (2N-1), +1 slack */
    scene->nodes = (PTBVHNode *)malloc(sizeof(PTBVHNode) * (size_t)max_nodes);
    scene->node_count = 0;
    bvh_build_recursive(scene->tris, 0, scene->tri_count, scene->nodes, &scene->node_count);
}

int pt_scene_intersect(const PTScene *scene, Vec3f origin, Vec3f dir, float t_max,
                        float *out_t, int *out_tri_index) {
    if (!scene->nodes || scene->node_count == 0) return 0;
    Vec3f inv_dir = v3(
        fabsf(dir.x) > 1e-12f ? 1.0f / dir.x : (dir.x >= 0 ? FLT_MAX : -FLT_MAX),
        fabsf(dir.y) > 1e-12f ? 1.0f / dir.y : (dir.y >= 0 ? FLT_MAX : -FLT_MAX),
        fabsf(dir.z) > 1e-12f ? 1.0f / dir.z : (dir.z >= 0 ? FLT_MAX : -FLT_MAX)
    );
    float best_t = t_max;
    int best_tri = -1;

    int stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        const PTBVHNode *node = &scene->nodes[ni];

        /* Slab test against this node's AABB, bounded by the best hit
         * found so far so the traversal keeps tightening. */
        float o[3] = { origin.x, origin.y, origin.z };
        float id[3] = { inv_dir.x, inv_dir.y, inv_dir.z };
        float t0 = 0.0f, t1 = best_t;
        int miss = 0;
        for (int a = 0; a < 3; a++) {
            float tmin = (node->bmin[a] - o[a]) * id[a];
            float tmax = (node->bmax[a] - o[a]) * id[a];
            if (tmin > tmax) { float tmp = tmin; tmin = tmax; tmax = tmp; }
            if (tmin > t0) t0 = tmin;
            if (tmax < t1) t1 = tmax;
            if (t0 > t1) { miss = 1; break; }
        }
        if (miss) continue;

        if (node->count > 0) {
            for (int i = node->start; i < node->start + node->count; i++) {
                float t;
                if (ray_intersect_triangle(origin, dir, scene->tris[i].v0, scene->tris[i].v1, scene->tris[i].v2, &t)) {
                    if (t < best_t) { best_t = t; best_tri = i; }
                }
            }
        } else {
            if (sp < 126) { stack[sp++] = node->left; stack[sp++] = node->right; }
        }
    }
    if (best_tri < 0) return 0;
    *out_t = best_t; *out_tri_index = best_tri;
    return 1;
}

/* ============================== BSDF ============================== */

static Vec3f mix3(Vec3f a, Vec3f b, float t) { return v3add(v3scale(a, 1.0f - t), v3scale(b, t)); }
static Vec3f fresnel_schlick(Vec3f f0, float vdoth) {
    float p = powf(1.0f - vdoth, 5.0f);
    return v3add(f0, v3scale(v3sub(v3(1,1,1), f0), p));
}
/* UE4's analytic-light remap (Karis 2013) for the Schlick-GGX visibility
 * term's k -- standard, not invented here, reused identically for both
 * direct-light evaluation (pt_eval_bsdf) and importance-sampled bounces
 * (pt_sample_bsdf) so both agree on the same BRDF. */
static float schlick_g1(float ndotx, float k) { return ndotx / (ndotx * (1.0f - k) + k); }

Vec3f pt_eval_bsdf(Vec3f wo, Vec3f wi, Vec3f n, Vec3f base_color, float metallic, float roughness) {
    float ndotv = v3dot(n, wo), ndotl = v3dot(n, wi);
    if (ndotv <= 0.0f || ndotl <= 0.0f) return v3(0,0,0);
    Vec3f h = v3norm(v3add(wo, wi));
    float ndoth = v3dot(n, h), vdoth = v3dot(wo, h);
    if (ndoth <= 0.0f) return v3(0,0,0);

    float alpha = roughness * roughness;
    if (alpha < 0.02f) alpha = 0.02f;
    float alpha2 = alpha * alpha;
    float denom = ndoth * ndoth * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / ((float)M_PI * denom * denom + 1e-9f);

    float k = (roughness + 1.0f); k = (k * k) / 8.0f;
    float G = schlick_g1(ndotv, k) * schlick_g1(ndotl, k);

    Vec3f F0 = mix3(v3(0.04f, 0.04f, 0.04f), base_color, metallic);
    Vec3f F = fresnel_schlick(F0, vdoth);

    Vec3f specular = v3scale(F, D * G / (4.0f * ndotv * ndotl + 1e-6f));
    Vec3f diffuse = v3scale(base_color, (1.0f - metallic) / (float)M_PI);
    return v3add(diffuse, specular);
}

/* Duff, Burgess, Christensen, Hery, Kensler, Liktor, Wilkie 2017 --
 * branchless orthonormal basis from a single unit vector, standard
 * technique, not derived here. */
static void onb(Vec3f n, Vec3f *t, Vec3f *b) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float bb = n.x * n.y * a;
    *t = v3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
    *b = v3(bb, sign + n.y * n.y * a, -n.y);
}
static Vec3f local_to_world(Vec3f local, Vec3f t, Vec3f b, Vec3f n) {
    return v3add(v3add(v3scale(t, local.x), v3scale(b, local.y)), v3scale(n, local.z));
}

Vec3f pt_sample_bsdf(unsigned int *rng, Vec3f wo, Vec3f n, Vec3f base_color, float metallic, float roughness,
                      Vec3f *out_dir) {
    /* Metallic-driven lobe-selection probability -- a smooth dielectric
     * (metallic=0) spends most samples on its dominant diffuse lobe but
     * still occasionally samples its real (if dim) dielectric specular
     * highlight (F0=0.04); a metal (metallic=1) has no diffuse lobe at
     * all (base_color*(1-metallic)=0) so spends almost all samples on
     * specular. Clamped away from 0/1 so neither lobe is ever completely
     * unreachable (avoids a hard bias, standard practice for stochastic
     * lobe selection). */
    float p_spec = 0.05f + 0.90f * metallic;
    if (p_spec < 0.05f) p_spec = 0.05f;
    if (p_spec > 0.95f) p_spec = 0.95f;

    Vec3f t, b;
    onb(n, &t, &b);

    if (rand01(rng) < p_spec) {
        float alpha = roughness * roughness;
        if (alpha < 0.02f) alpha = 0.02f;
        float xi1 = rand01(rng), xi2 = rand01(rng);
        float costheta = sqrtf((1.0f - xi1) / (1.0f + (alpha*alpha - 1.0f) * xi1));
        float sintheta = sqrtf(fmaxf(0.0f, 1.0f - costheta*costheta));
        float phi = 2.0f * (float)M_PI * xi2;
        Vec3f h_local = v3(sintheta * cosf(phi), sintheta * sinf(phi), costheta);
        Vec3f h = local_to_world(h_local, t, b, n);

        Vec3f wi = v3sub(v3scale(h, 2.0f * v3dot(wo, h)), wo);
        *out_dir = wi;
        float ndotl = v3dot(n, wi), ndotv = v3dot(n, wo), ndoth = v3dot(n, h), vdoth = v3dot(wo, h);
        if (ndotl <= 0.0f || ndotv <= 0.0f || ndoth <= 0.0f || vdoth <= 0.0f) return v3(0,0,0);

        float k = (roughness + 1.0f); k = (k * k) / 8.0f;
        float G = schlick_g1(ndotv, k) * schlick_g1(ndotl, k);
        Vec3f F0 = mix3(v3(0.04f, 0.04f, 0.04f), base_color, metallic);
        Vec3f F = fresnel_schlick(F0, vdoth);
        /* f_spec*NdotL/pdf simplifies to G*F*VdotH/(NdotV*NdotH) when
         * sampling h from the GGX distribution directly (Walter et al.
         * 2007's standard microfacet-sampling result) -- not re-derived
         * from scratch here, the well-known closed form. */
        Vec3f weight = v3scale(F, G * vdoth / (ndotv * ndoth * p_spec + 1e-9f));
        return weight;
    } else {
        float xi1 = rand01(rng), xi2 = rand01(rng);
        float r = sqrtf(xi1), theta = 2.0f * (float)M_PI * xi2;
        Vec3f local = v3(r * cosf(theta), r * sinf(theta), sqrtf(fmaxf(0.0f, 1.0f - xi1)));
        Vec3f wi = local_to_world(local, t, b, n);
        *out_dir = wi;
        /* Cosine-weighted sampling makes f*cos/pdf collapse to the flat
         * albedo -- see this function's own header comment / the
         * standalone furnace test. */
        return v3scale(base_color, (1.0f - metallic) / (1.0f - p_spec));
    }
}

/* ============================== Direct lighting (NEE) ============================== */

/* Shared by pt_direct_light_contribution and pt_render's own shadow-ray
 * step, so there is exactly one place that derives "which direction, how
 * far" per light type -- the two callers can never silently drift apart
 * on that. Returns 0 if this light cannot possibly illuminate p (e.g.
 * degenerate zero-distance point light). out_light_normal is only
 * meaningful for AREA lights (the emitting face's outward normal). */
static int light_sample_dir(const PhiLight *light, Vec3f p, Vec3f *out_wi, float *out_dist, Vec3f *out_light_normal) {
    if (light->type == LIGHT_TYPE_SUN) {
        Vec3f dir = v3norm(light->direction);
        *out_wi = v3neg(dir);
        *out_dist = 1e6f;   /* effectively unbounded -- sun has no real distance */
        if (out_light_normal) *out_light_normal = v3neg(dir);
        return 1;
    }
    Vec3f to_light = v3sub(light->position, p);
    float d = v3len(to_light);
    if (d < 1e-6f) return 0;
    *out_wi = v3scale(to_light, 1.0f / d);
    *out_dist = d;
    if (out_light_normal) *out_light_normal = v3norm(light->direction);
    return 1;
}

Vec3f pt_direct_light_contribution(const PhiLight *light, Vec3f p, Vec3f n, Vec3f wo,
                                    Vec3f base_color, float metallic, float roughness) {
    Vec3f wi, light_n;
    float dist;
    if (!light_sample_dir(light, p, &wi, &dist, &light_n)) return v3(0,0,0);
    float ndotl = v3dot(n, wi);
    if (ndotl <= 0.0f) return v3(0,0,0);

    float irradiance = 0.0f;
    if (light->type == LIGHT_TYPE_SUN) {
        /* Blender's own Sun convention: Strength is already an irradiance
         * (W/m^2) hitting a surface perpendicular to the sun, not a power
         * -- no inverse-square falloff (the sun is modeled as infinitely
         * far away). */
        irradiance = light->energy;
    } else if (light->type == LIGHT_TYPE_POINT) {
        /* Point light: energy is radiant power (W). Radiant intensity of
         * an isotropic point emitter is I = power/(4*pi); irradiance at
         * distance d is I/d^2 -- the standard point-light radiometry
         * every real renderer (Blender/PBRT/etc.) uses for a Watts-based
         * point light. */
        irradiance = light->energy / (4.0f * (float)M_PI * dist * dist);
    } else if (light->type == LIGHT_TYPE_SPOT) {
        float cos_outer = cosf(light->spot_size * 0.5f);
        float cos_inner = cosf(light->spot_size * 0.5f * (1.0f - light->spot_blend));
        float cos_to_point = v3dot(v3norm(light->direction), v3neg(wi));
        float atten;
        if (cos_inner <= cos_outer) atten = (cos_to_point >= cos_outer) ? 1.0f : 0.0f;
        else {
            atten = (cos_to_point - cos_outer) / (cos_inner - cos_outer);
            if (atten < 0.0f) atten = 0.0f;
            if (atten > 1.0f) atten = 1.0f;
            atten = atten * atten * (3.0f - 2.0f * atten);  /* smoothstep, matches Blender's own spot-edge softening shape */
        }
        irradiance = (light->energy / (4.0f * (float)M_PI * dist * dist)) * atten;
    } else { /* LIGHT_TYPE_AREA */
        /* Single-representative-point approximation (light->position IS
         * the sample, not a randomly jittered point within the square) --
         * a real, honest scope limit: this gives correct MEAN energy but
         * no soft-shadow penumbra from the area's own size (which needs
         * stratified multi-point area sampling, not attempted this pass).
         * Derivation: for a diffuse (Lambertian) emitter of total power
         * `energy` over `area`, exitant radiance Le = energy/(pi*area);
         * single-point solid-angle-form contribution divides by
         * distance^2 and multiplies by the emitting face's own cosine
         * (cos_light) and by `area` -- the `area` factor exactly cancels
         * the 1/area inside Le, so the result is independent of area_size
         * (physically sensible: the same total power spread over a
         * differently-sized face contributes the same total light). */
        float cos_light = v3dot(light_n, v3neg(wi));
        if (cos_light <= 0.0f) return v3(0,0,0);
        irradiance = (light->energy / ((float)M_PI * dist * dist)) * cos_light;
    }
    if (irradiance <= 0.0f) return v3(0,0,0);

    Vec3f f = pt_eval_bsdf(wo, wi, n, base_color, metallic, roughness);
    return v3scale(v3mul(f, light->color), irradiance * ndotl);
}

/* ============================== Camera + render ============================== */

/* Matches main.c's own cam_basis EXACTLY (formula-for-formula, verified
 * against that function directly rather than re-derived independently)
 * so a path-traced render lines up with whatever the live rasterizer
 * shows from the same yaw/pitch. */
static void pt_cam_basis(float yaw, float pitch, Vec3f *fwd, Vec3f *right, Vec3f *up) {
    float sy = sinf(yaw), cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    *fwd   = v3(-sy*cp, sp, -cy*cp);
    *right = v3(cy, 0.0f, -sy);
    *up    = v3(sy*sp, cp, cy*sp);
}

float *pt_render(const PTScene *scene, PhiLight *const *lights, int n_lights,
                  Vec3f cam_pos, float cam_yaw, float cam_pitch, float fov_y,
                  const PTRenderParams *params) {
    if (!params || params->width <= 0 || params->height <= 0 || params->samples <= 0) return NULL;
    int w = params->width, h = params->height;
    int max_bounces = params->max_bounces > 0 ? params->max_bounces : 6;
    float *out = (float *)calloc((size_t)w * (size_t)h * 3, sizeof(float));

    Vec3f fwd, right, up;
    pt_cam_basis(cam_yaw, cam_pitch, &fwd, &right, &up);
    float half_h = tanf(fov_y * 0.5f);
    float half_w = half_h * ((float)w / (float)h);
    /* Dim, flat sky ambient for camera rays that escape the scene
     * entirely -- deliberately simple (no HDRI/environment-map lighting,
     * real future work, not attempted this pass), just enough that a
     * miss isn't pure black. */
    const Vec3f sky = v3(0.05f, 0.07f, 0.10f);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned int rng = (unsigned int)(y * 9781 + x * 6151 + 1) * 2654435761u;
            if (rng == 0) rng = 1;
            Vec3f accum = v3(0,0,0);

            for (int s = 0; s < params->samples; s++) {
                float jx = rand01(&rng) - 0.5f, jy = rand01(&rng) - 0.5f;
                float ndc_x = 2.0f * ((float)x + 0.5f + jx) / (float)w - 1.0f;
                float ndc_y = 1.0f - 2.0f * ((float)y + 0.5f + jy) / (float)h;
                Vec3f dir = v3norm(v3add(fwd, v3add(v3scale(right, ndc_x * half_w), v3scale(up, ndc_y * half_h))));

                Vec3f ray_o = cam_pos, ray_d = dir;
                Vec3f radiance = v3(0,0,0), throughput = v3(1,1,1);

                for (int bounce = 0; bounce < max_bounces; bounce++) {
                    float t; int tri_idx;
                    if (!pt_scene_intersect(scene, ray_o, ray_d, 1e30f, &t, &tri_idx)) {
                        radiance = v3add(radiance, v3mul(throughput, sky));
                        break;
                    }
                    const PTTriangle *tri = &scene->tris[tri_idx];
                    Vec3f p = v3add(ray_o, v3scale(ray_d, t));
                    Vec3f n = tri->n;
                    Vec3f wo = v3neg(ray_d);
                    if (v3dot(n, wo) < 0.0f) n = v3neg(n);  /* shade whichever side the ray actually approached from */

                    /* Emissive triangles: safe to add unconditionally
                     * here -- NEE below only ever samples dedicated Light
                     * objects, never emissive geometry, so there is no
                     * double-counting to guard against (see path_tracer.h's
                     * own note on this). */
                    radiance = v3add(radiance, v3mul(throughput, tri->emission));

                    Vec3f p_off = v3add(p, v3scale(n, 1e-4f));
                    for (int li = 0; li < n_lights; li++) {
                        const PhiLight *light = lights[li];
                        if (!light) continue;
                        Vec3f contrib = pt_direct_light_contribution(light, p, n, wo, tri->base_color, tri->metallic, tri->roughness);
                        if (contrib.x <= 0.0f && contrib.y <= 0.0f && contrib.z <= 0.0f) continue;
                        Vec3f wi; float dist; Vec3f ln;
                        if (!light_sample_dir(light, p, &wi, &dist, &ln)) continue;
                        float shadow_t; int shadow_tri;
                        if (pt_scene_intersect(scene, p_off, wi, dist - 2e-4f, &shadow_t, &shadow_tri)) continue; /* occluded */
                        radiance = v3add(radiance, v3mul(throughput, contrib));
                    }

                    Vec3f new_dir;
                    Vec3f weight = pt_sample_bsdf(&rng, wo, n, tri->base_color, tri->metallic, tri->roughness, &new_dir);
                    throughput = v3mul(throughput, weight);
                    if (throughput.x <= 0.0f && throughput.y <= 0.0f && throughput.z <= 0.0f) break;

                    if (bounce >= 3) {
                        float p_continue = v3max3(throughput);
                        if (p_continue < 0.05f) p_continue = 0.05f;
                        if (p_continue > 0.95f) p_continue = 0.95f;
                        if (rand01(&rng) > p_continue) break;
                        throughput = v3scale(throughput, 1.0f / p_continue);
                    }

                    ray_o = v3add(p, v3scale(n, 1e-4f));
                    ray_d = new_dir;
                }
                accum = v3add(accum, radiance);
            }
            accum = v3scale(accum, 1.0f / (float)params->samples);
            size_t px = ((size_t)y * (size_t)w + (size_t)x) * 3;
            out[px+0] = accum.x; out[px+1] = accum.y; out[px+2] = accum.z;
        }
    }
    return out;
}

int pt_write_png(const char *path, const float *linear_rgb, int width, int height) {
    unsigned char *bytes = (unsigned char *)malloc((size_t)width * (size_t)height * 3);
    for (size_t i = 0; i < (size_t)width * (size_t)height * 3; i++) {
        float c = linear_rgb[i];
        c = c / (1.0f + c);              /* Reinhard tonemap -- real HDR-to-LDR compression, not a raw clamp */
        c = powf(fmaxf(0.0f, c), 1.0f / 2.2f);  /* gamma 2.2 */
        int v = (int)(c * 255.0f + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        bytes[i] = (unsigned char)v;
    }
    int ok = stbi_write_png(path, width, height, 3, bytes, width * 3);
    free(bytes);
    return ok;
}
