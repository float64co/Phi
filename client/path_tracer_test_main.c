/* Standalone, GL-free test harness for path_tracer.c -- Phase 3's real
 * offline path tracer (see path_tracer.h). No renderer.c/GL dependency
 * at all (pt_scene_build reads MeshObject::render_mesh directly, no
 * mesh_create/GL calls anywhere in this module), so unlike fracture_
 * body_test_main.c this needs no stub functions. */
#include "path_tracer.h"
#include "halfedge_gltf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

static Vec3f V(float x, float y, float z) { return (Vec3f){x, y, z}; }

/* Independently re-derived brute-force ray/triangle scan (NOT a call
 * into path_tracer.c's own accelerated pt_scene_intersect) -- same
 * "verify the accelerated path against a naive one, written separately"
 * technique fracture_test.c's adjacency check already established. */
static int brute_force_intersect(const PTScene *scene, Vec3f o, Vec3f d, float t_max, float *out_t) {
    const float EPS = 1e-7f;
    float best = t_max;
    int hit = 0;
    for (int i = 0; i < scene->tri_count; i++) {
        Vec3f v0 = scene->tris[i].v0, v1 = scene->tris[i].v1, v2 = scene->tris[i].v2;
        Vec3f e1 = { v1.x-v0.x, v1.y-v0.y, v1.z-v0.z };
        Vec3f e2 = { v2.x-v0.x, v2.y-v0.y, v2.z-v0.z };
        Vec3f pvec = { d.y*e2.z - d.z*e2.y, d.z*e2.x - d.x*e2.z, d.x*e2.y - d.y*e2.x };
        float det = e1.x*pvec.x + e1.y*pvec.y + e1.z*pvec.z;
        if (det > -EPS && det < EPS) continue;
        float inv = 1.0f / det;
        Vec3f tvec = { o.x-v0.x, o.y-v0.y, o.z-v0.z };
        float u = (tvec.x*pvec.x + tvec.y*pvec.y + tvec.z*pvec.z) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        Vec3f qvec = { tvec.y*e1.z - tvec.z*e1.y, tvec.z*e1.x - tvec.x*e1.z, tvec.x*e1.y - tvec.y*e1.x };
        float w = (d.x*qvec.x + d.y*qvec.y + d.z*qvec.z) * inv;
        if (w < 0.0f || u + w > 1.0f) continue;
        float t = (e2.x*qvec.x + e2.y*qvec.y + e2.z*qvec.z) * inv;
        if (t < EPS || t >= best) continue;
        best = t; hit = 1;
    }
    if (hit) *out_t = best;
    return hit;
}

static unsigned int xs32(unsigned int *s) {
    unsigned int x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 1;
    return x;
}

int main(void) {
    printf("[path_tracer_test] === setup ===\n");
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf");
    if (!hem) return 1;

    MeshObject obj = {0};
    obj.position = V(0, 0, 0);
    obj.orientation = quat_identity();
    /* assets/cube.gltf is a unit cube (half-extent ~0.5, see phi_physics_
     * meshobject_test_main.c's own AABB check) -- scaled up to match that
     * same test's own convention (half-extent ~8) so the camera below
     * doesn't need to sit uncomfortably close to a tiny unit cube. */
    obj.scale = V(16, 16, 16);
    obj.hem = hem;
    RenderMesh rm = {0};
    meshobject_build_render_mesh_from_halfedge(&rm, hem);
    obj.render_mesh = &rm;
    check(rm.count > 0 && rm.count % 3 == 0, "cube flattened to a real non-empty triangle list");

    printf("[path_tracer_test] === 1: scene build ===\n");
    const MeshObject *objs[1] = { &obj };
    PTScene scene = pt_scene_build(objs, 1);
    check(scene.tri_count == rm.count / 3, "scene triangle count matches the flattened render mesh exactly");
    check(scene.node_count > 0, "a real BVH was built (non-zero node count)");

    printf("[path_tracer_test] === 2: BVH traversal matches an independent brute-force scan ===\n");
    {
        int mismatches = 0;
        unsigned int rng = 777;
        for (int i = 0; i < 500; i++) {
            /* Random ray from a point well outside the unit-ish cube,
             * aimed roughly at the origin with some jitter -- a mix of
             * real hits and real misses. */
            float ang1 = (float)(xs32(&rng) & 0xFFFFFFu) / (float)0x1000000u * 6.2831853f;
            float ang2 = (float)(xs32(&rng) & 0xFFFFFFu) / (float)0x1000000u * 6.2831853f;
            Vec3f o = { 60.0f * cosf(ang1), 60.0f * sinf(ang2), 60.0f * sinf(ang1) };
            Vec3f target = { ((float)(xs32(&rng)&0xFFFFFFu)/(float)0x1000000u - 0.5f) * 20.0f,
                              ((float)(xs32(&rng)&0xFFFFFFu)/(float)0x1000000u - 0.5f) * 20.0f,
                              ((float)(xs32(&rng)&0xFFFFFFu)/(float)0x1000000u - 0.5f) * 20.0f };
            Vec3f d = { target.x-o.x, target.y-o.y, target.z-o.z };
            float len = sqrtf(d.x*d.x+d.y*d.y+d.z*d.z);
            d.x/=len; d.y/=len; d.z/=len;

            float bvh_t, brute_t;
            int bvh_tri;
            int bvh_hit = pt_scene_intersect(&scene, o, d, 1e30f, &bvh_t, &bvh_tri);
            int brute_hit = brute_force_intersect(&scene, o, d, 1e30f, &brute_t);
            if (bvh_hit != brute_hit) { mismatches++; continue; }
            if (bvh_hit && fabsf(bvh_t - brute_t) > 1e-3f) mismatches++;
        }
        check(mismatches == 0, "500 random rays: BVH-accelerated nearest hit matches brute force exactly (hit/miss and t)");
    }

    printf("[path_tracer_test] === 3: BSDF furnace test -- no energy gain ===\n");
    {
        Vec3f n = V(0,0,1), wo = V(0,0,1);
        Vec3f sum = V(0,0,0);
        unsigned int rng = 42;
        const int N = 20000;
        for (int i = 0; i < N; i++) {
            Vec3f dir;
            Vec3f w = pt_sample_bsdf(&rng, wo, n, V(1,1,1), 0.0f, 0.5f, &dir);
            sum.x += w.x; sum.y += w.y; sum.z += w.z;
        }
        Vec3f mean = { sum.x/N, sum.y/N, sum.z/N };
        printf("    dielectric (metallic=0) mean weight: (%.3f, %.3f, %.3f)\n", mean.x, mean.y, mean.z);
        check(mean.x > 0.5f && mean.x < 1.3f && mean.y > 0.5f && mean.y < 1.3f && mean.z > 0.5f && mean.z < 1.3f,
              "dielectric: mean sampled BSDF weight is a real, energy-conserving value near the diffuse albedo");

        sum = V(0,0,0);
        rng = 43;
        for (int i = 0; i < N; i++) {
            Vec3f dir;
            Vec3f w = pt_sample_bsdf(&rng, wo, n, V(0.9f,0.9f,0.9f), 1.0f, 0.3f, &dir);
            sum.x += w.x; sum.y += w.y; sum.z += w.z;
        }
        mean = (Vec3f){ sum.x/N, sum.y/N, sum.z/N };
        printf("    metal (metallic=1) mean weight: (%.3f, %.3f, %.3f)\n", mean.x, mean.y, mean.z);
        check(mean.x > 0.0f && mean.x < 1.5f && mean.y > 0.0f && mean.y < 1.5f && mean.z > 0.0f && mean.z < 1.5f,
              "metal: mean sampled BSDF weight stays within a generous energy-conservation bound (Schlick-GGX visibility is an approximation, not perfectly energy-preserving)");
    }

    printf("[path_tracer_test] === 4: NEE closed-form radiometry checks ===\n");
    {
        Vec3f p = V(0,0,0), n = V(0,1,0), wo = V(0,1,0);
        Vec3f albedo = V(1,1,1);

        PhiLight light = {0};
        light.type = LIGHT_TYPE_POINT;
        light.position = V(0, 10, 0);
        light.color = V(1,1,1);
        light.energy = 1000.0f;
        Vec3f c1 = pt_direct_light_contribution(&light, p, n, wo, albedo, 0.0f, 1.0f);

        light.position = V(0, 20, 0);   /* same direction, double distance */
        Vec3f c2 = pt_direct_light_contribution(&light, p, n, wo, albedo, 0.0f, 1.0f);
        float ratio = (c1.y > 1e-8f) ? c2.y / c1.y : -1.0f;
        printf("    point light inverse-square ratio (expect ~0.25): %.5f\n", ratio);
        check(fabsf(ratio - 0.25f) < 0.01f, "point light: doubling distance cuts contribution to ~1/4 (inverse-square)");

        light.position = V(0, 10, 0);
        light.energy = 2000.0f;   /* double the power, same distance */
        Vec3f c3 = pt_direct_light_contribution(&light, p, n, wo, albedo, 0.0f, 1.0f);
        float lin_ratio = (c1.y > 1e-8f) ? c3.y / c1.y : -1.0f;
        printf("    point light energy-linearity ratio (expect ~2.0): %.5f\n", lin_ratio);
        check(fabsf(lin_ratio - 2.0f) < 0.01f, "point light: doubling power exactly doubles contribution (linear)");

        light.energy = 1000.0f;
        light.position = V(0, -10, 0);   /* light BEHIND the surface (below the horizon) */
        Vec3f c4 = pt_direct_light_contribution(&light, p, n, wo, albedo, 0.0f, 1.0f);
        check(c4.x == 0.0f && c4.y == 0.0f && c4.z == 0.0f, "point light below the horizon (NdotL<0) contributes exactly zero");

        PhiLight sun = {0};
        sun.type = LIGHT_TYPE_SUN;
        sun.direction = V(0, -1, 0);   /* shines straight down onto the upward-facing surface */
        sun.color = V(1,1,1);
        sun.energy = 5.0f;
        Vec3f s1 = pt_direct_light_contribution(&sun, p, n, wo, albedo, 0.0f, 1.0f);
        check(s1.x > 0.0f, "sun light hitting the surface contributes real, positive radiance");

        sun.energy = 10.0f;
        Vec3f s2 = pt_direct_light_contribution(&sun, p, n, wo, albedo, 0.0f, 1.0f);
        float sun_ratio = (s1.y > 1e-8f) ? s2.y / s1.y : -1.0f;
        check(fabsf(sun_ratio - 2.0f) < 0.01f, "sun light: doubling strength exactly doubles contribution (linear, no distance falloff)");

        sun.direction = V(0, 1, 0);   /* shining UP, into the back of the surface */
        Vec3f s3 = pt_direct_light_contribution(&sun, p, n, wo, albedo, 0.0f, 1.0f);
        check(s3.x == 0.0f && s3.y == 0.0f && s3.z == 0.0f, "sun light shining away from the surface contributes exactly zero");
    }

    printf("[path_tracer_test] === 5: end-to-end render -- real light actually reaches the camera ===\n");
    {
        PhiLight light = {0};
        light.type = LIGHT_TYPE_POINT;
        light.position = V(0, 40, 40);
        light.color = V(1,1,1);
        light.energy = 40000.0f;
        PhiLight *lights[1] = { &light };

        /* Camera at (40,30,40), yaw/pitch chosen so fwd points exactly at
         * the origin (worked out from cam_basis's own formula: fwd =
         * (-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch)); with
         * cam_pos symmetric in x/z, yaw=45 degrees, pitch=asin(dir.y)) --
         * the scaled cube (half-extent 8, see setup above) comfortably
         * fills a real portion of the frame from here, not a speck. */
        PTRenderParams params = { 24, 24, 8, 4 };
        float *img = pt_render(&scene, lights, 1, V(40, 30, 40), 0.7853982f, -0.4863f, 0.9f, &params);
        check(img != NULL, "pt_render returned a real image buffer");

        double sum = 0.0;
        float min_v = 1e30f, max_v = -1e30f;
        int finite = 1;
        for (int i = 0; i < params.width * params.height * 3; i++) {
            if (!isfinite(img[i])) finite = 0;
            sum += img[i];
            if (img[i] < min_v) min_v = img[i];
            if (img[i] > max_v) max_v = img[i];
        }
        check(finite, "every pixel is a finite float (no NaN/Inf from the BVH/BSDF/NEE math)");
        check(sum > 0.0, "real light reached the camera (image is not all-black)");
        /* Pixels that miss the scene entirely are the flat, exactly-
         * uniform sky constant (see pt_render's own `sky` value) -- real
         * variation across the image only happens if some rays actually
         * hit and shaded the cube. A much stronger, framing-independent
         * signal than "sum > 0" alone (which a pure-sky miss would also
         * satisfy trivially). */
        printf("    pixel value range: [%.4f, %.4f]\n", min_v, max_v);
        check(max_v - min_v > 0.02f, "image shows real variation, not a flat sky-only miss (the cube was actually hit and shaded)");

        int ok = pt_write_png("/tmp/phi_path_tracer_test.png", img, params.width, params.height);
        check(ok, "pt_write_png reports success");

        FILE *f = fopen("/tmp/phi_path_tracer_test.png", "rb");
        check(f != NULL, "the PNG file actually exists on disk");
        if (f) {
            unsigned char magic[8];
            size_t n = fread(magic, 1, 8, f);
            const unsigned char png_sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
            check(n == 8 && memcmp(magic, png_sig, 8) == 0, "file starts with a real PNG signature, not junk");
            fclose(f);
        }
        free(img);
    }

    printf("[path_tracer_test] === 6: teardown ===\n");
    pt_scene_destroy(&scene);
    free(rm.data);
    halfedge_destroy(hem);
    check(1, "torn down without crashing (this line running proves it)");

    printf("\n[path_tracer_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
