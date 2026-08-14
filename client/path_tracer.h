#pragma once
#include "vec3.h"
#include "meshobject.h"
#include "light.h"
#include "render_settings.h"

/* Phase 3's real offline path tracer (see phi.md's Phase 3 status) -- a
 * genuine BVH-accelerated Monte Carlo integrator over the live scene's
 * actual geometry (whatever MeshObject(s) main.c currently has on
 * screen -- the single test object, or its live fracture fragments once
 * activated, see fracture_body.h), with a real microfacet BSDF (Lambertian
 * diffuse + GGX specular, chosen by the same base_color/metallic/
 * roughness fields the live rasterizer and Properties panel already
 * read/write) and next-event estimation against the real Light registry
 * (light.h). Scoped first to a single still frame, PNG output (via the
 * newly-vendored stb_image_write.h, same "vendor a small proven single-
 * header library" precedent as stb_truetype.h/cgltf.h) -- an animation-
 * sequence + ffmpeg video export is explicit deferred follow-up, not
 * attempted here.
 *
 * NOT wired into the live deferred rasterizer -- entirely separate code
 * path (its own ray/BVH/BSDF math, no GL calls at all), same "genuinely
 * offline, not a real-time approximation" split every real path tracer
 * in a hybrid engine (Blender's Cycles vs. EEVEE, Unreal's Path Tracer
 * vs. Lumen) draws. */

typedef struct {
    Vec3f v0, v1, v2;
    Vec3f n;             /* flat face normal, normalized, world space */
    Vec3f base_color;
    float metallic;
    float roughness;
    Vec3f emission;
} PTTriangle;

typedef struct PTBVHNode {
    float bmin[3], bmax[3];
    int   left, right;   /* child node indices into the owning PTScene's nodes[], -1 if this is a leaf */
    int   start, count;  /* leaf only: range into scene->tris[] */
} PTBVHNode;

typedef struct {
    PTTriangle  *tris;
    int          tri_count;
    PTBVHNode   *nodes;
    int          node_count;
} PTScene;

/* Builds a PTScene from a flat list of world-ready MeshObjects (each
 * must already have a real render_mesh -- both the live test object and
 * fracture_body fragments qualify, see fracture_body_get_object) by
 * flattening every triangle into world space (matching renderer.c's own
 * T*R*S model matrix exactly, see renderer_draw_mesh_object -- position,
 * quat_to_mat4(orientation), then component-wise scale) and building a
 * median-split BVH over the result. Returns a scene with tri_count==0
 * (still a valid, empty, safely-destroyable PTScene) if every object is
 * empty or n_objects is 0 -- not a NULL/failure case, since an empty
 * scene is a real, renderable (all-background) result. */
PTScene pt_scene_build(const MeshObject *const *objects, int n_objects);
void    pt_scene_destroy(PTScene *scene);

/* Nearest-hit ray/scene intersection through the BVH -- returns 1 and
 * fills out_t/out_tri_index on a hit, 0 on a miss. t_max bounds the
 * search (pass a large value, e.g. 1e30f, for an unbounded ray; shadow
 * rays pass the real distance to the light instead so an occluder
 * beyond the light doesn't falsely shadow it). Exposed directly (not
 * just used internally by pt_render) so a standalone test can check it
 * against an independently-written brute-force loop over scene->tris,
 * the same "verify the accelerated path against a naive one" technique
 * fracture_test.c's adjacency check already established. */
int pt_scene_intersect(const PTScene *scene, Vec3f origin, Vec3f dir, float t_max,
                        float *out_t, int *out_tri_index);

/* ---- BSDF -- Lambertian diffuse + GGX specular, real microfacet math
 * (Cook-Torrance form: D * G * F / (4 NdotV NdotL)), not a toy Phong
 * approximation. wo/wi/n are all unit world-space directions, wo/wi both
 * point AWAY from the shaded point (standard rendering-equation
 * convention -- wi is NOT "toward the surface"). Specular color (F0) is
 * mix(0.04, base_color, metallic), the same real-time-PBR convention
 * this project's Properties-panel material model already documents
 * (halfedge.h's own material field comments). Exposed for the standalone
 * furnace test (see path_tracer_test_main.c) as well as genuinely useful
 * on their own, e.g. for direct-lighting evaluation below. ---- */
Vec3f pt_eval_bsdf(Vec3f wo, Vec3f wi, Vec3f n, Vec3f base_color, float metallic, float roughness);

/* Stochastically samples ONE lobe (diffuse via cosine-weighted
 * hemisphere sampling, or specular via GGX half-vector importance
 * sampling, chosen with a metallic-driven probability so a smooth
 * dielectric doesn't waste most samples on its dim specular lobe and a
 * metal doesn't waste most samples on a lobe it barely has), sets
 * *out_dir to the sampled wi, and returns the already-divided-by-pdf
 * throughput weight (f(wo,wi) * |NdotL| / pdf(wi), ALSO divided by that
 * lobe's own selection probability) -- multiply a path's running
 * throughput by this directly, no separate pdf bookkeeping needed by
 * the caller. Unbiased: see path_tracer_test_main.c's furnace test
 * (average returned weight, over many samples, never exceeds the
 * physically-conserved energy bound). */
Vec3f pt_sample_bsdf(unsigned int *rng, Vec3f wo, Vec3f n, Vec3f base_color, float metallic, float roughness,
                      Vec3f *out_dir);

/* Real, closed-form (no stochastic sampling, no noise) next-event-
 * estimation contribution from a single Light at world point p (with
 * shading normal n and BSDF params) toward view direction wo, IGNORING
 * occlusion -- callers doing an actual render test a shadow ray
 * themselves (see pt_render's own use of this) before trusting the
 * result; exposed unoccluded so a standalone test can check the
 * lighting math itself (inverse-square falloff, linearity in power,
 * spot cone attenuation, sun's distance-independent irradiance) in
 * exact closed form, decoupled from any scene/shadow-ray noise. See
 * this function's own header comment in path_tracer.c for the real
 * per-type radiometry (point/spot: power->intensity->irradiance; sun:
 * direct irradiance, no falloff; area: single representative-point
 * sample of the emitting face, not full stratified-area sampling --
 * flagged honestly as this pass's real scope limit on area lights). */
Vec3f pt_direct_light_contribution(const PhiLight *light, Vec3f p, Vec3f n, Vec3f wo,
                                    Vec3f base_color, float metallic, float roughness);

typedef struct {
    int width, height;
    int samples;        /* render_settings.h's RenderSettings::samples -- caller passes g_render_settings.samples through */
    int max_bounces;    /* real path length cap, Russian-roulette-terminated earlier on low-throughput paths, see path_tracer.c */
} PTRenderParams;

/* Renders one still frame of scene as seen from a camera at cam_pos with
 * the SAME yaw/pitch/fov_y basis convention main.c's cam_basis/
 * compute_scene_ray already use (verified against that formula directly,
 * not re-derived independently, so a path-traced render lines up with
 * what the live rasterizer shows from the same camera state) against
 * every live Light in the registry (light_get_all). Returns a malloc'd
 * linear-HDR float buffer, width*height*3 floats, row-major top-to-
 * bottom (caller frees) -- NOT yet tonemapped/gamma-corrected, see
 * pt_write_png for that step. Returns NULL only if width/height/samples
 * are non-positive (a real argument-validation failure, not "the scene
 * was empty" -- an empty scene still renders a valid, all-background
 * image). */
float *pt_render(const PTScene *scene, PhiLight *const *lights, int n_lights,
                  Vec3f cam_pos, float cam_yaw, float cam_pitch, float fov_y,
                  const PTRenderParams *params);

/* Reinhard-tonemaps + gamma-2.2-corrects linear_rgb (width*height*3
 * floats, same layout pt_render returns) into 8-bit RGB and writes a
 * real PNG via stb_image_write's stbi_write_png. Returns 1 on success, 0
 * on a real write failure (bad path, out of disk, etc. -- stb_image_
 * write's own return convention). */
int pt_write_png(const char *path, const float *linear_rgb, int width, int height);
