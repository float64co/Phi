#pragma once
#include "octree_render.h"  /* RenderMesh, for gbuffer_render_shadow_map */
#include "vec3.h"           /* Vec3f, for gbuffer_set_point_lights */

/* Small, fixed cap on real point lights the lighting pass sums per pixel
 * (see gbuffer_set_point_lights/LIGHTING_FRAG_SRC) -- a plain uniform-
 * array loop, not tiled/clustered lighting, so this needs to stay small
 * enough that a per-pixel loop over all of them is cheap; 8 is generous
 * for this project's actual scene sizes (a handful of real lights, not
 * hundreds) without needing that machinery. */
#define GBUF_MAX_POINT_LIGHTS 8

/* Deferred renderer G-buffer (Phase 0 — see phi.md's "Deferred Renderer
 * and G-Buffer" section for the target layout this implements). Compiles
 * for both native (GL 3.3 core via gl_native.h proc-fetching) and wasm
 * (GLES3/gl3.h, directly linked — Emscripten declares all of MRT/FBO/VAO
 * as real functions, no proc-fetching needed there) from the same source;
 * confirmed working identically on Linux native, Windows native, and
 * wasm/WebGL2 in a real browser.
 *
 * Stages, matching the pipeline diagram in phi.md:
 *   [geometry] -> writes this G-buffer            (gbuffer_begin_geometry_pass,
 *                                                    then the existing
 *                                                    renderer_draw_* calls)
 *   [shadow]   -> depth-only from the light's POV  (gbuffer_render_shadow_map)
 *   [lighting] -> HDR accumulation (RGBA16F)       (gbuffer_resolve, part 1)
 *   [bloom]    -> threshold + separable blur,      (gbuffer_resolve, part 2 —
 *                 additive-composited back into HDR  see the honest caveat on
 *                                                     tex_bright below: correct,
 *                                                     working plumbing with
 *                                                     nothing to bloom yet
 *                                                     under current content)
 *   [transparency] -> forward-blended, depth-tested   (gbuffer_resolve, part 3 —
 *                     but not depth-writing, into HDR    see the honest caveat
 *                                                         on transparent_test_vbo
 *                                                         below: a fixed test
 *                                                         quad, since nothing
 *                                                         transparent exists in
 *                                                         the game yet)
 *   [tonemap]  -> HDR -> intermediate LDR texture  (gbuffer_resolve, part 4)
 *   [taa]      -> temporal resolve against history  (gbuffer_resolve, part 5 —
 *                 (velocity-reprojected, neighborhood-  see taa_tex_a/b's
 *                 clamped), LDR -> LDR                  comment for the
 *                                                        camera-motion-only
 *                                                        velocity caveat)
 *   [fxaa]     -> LDR texture -> default framebuffer (gbuffer_resolve, part 6)
 *
 * Not implemented yet, left for later: the Python @phi.render_pass
 * insertion-point system (needs MicroPython, Phase 5, not started —
 * deliberately not stubbed out early, see phi.md) — this is still not the
 * finished pipeline, just a bigger first pass than before. */

typedef struct {
    int w, h;
    /* Where the final resolved frame lands in the actual window — see
     * gbuffer_set_viewport_offset(). Default (0,0): fills the window from
     * its origin, the only behavior this had before the UI system's Scene
     * panel needed to host the 3D view in an arbitrary sub-rectangle. */
    int vp_x, vp_y;

    unsigned int fbo;
    unsigned int tex_albedo;       /* RGBA8: base color (RGB) + AO (A, unused yet, always 1.0) */
    unsigned int tex_normal;       /* RGB10_A2: world normal *0.5+0.5 (RGB) + metallic (A, unused yet) */
    unsigned int tex_material;     /* RGBA8: R=metallic, G=roughness (B/A spare) — real per-face values for MeshObjects (renderer.c's PBR shader), a neutral 0/1 dielectric-rough default from the shared world/ground/players/rockets shader */
    unsigned int tex_emissive;     /* R11F_G11F_B10F: real per-face emission for MeshObjects, always black from the shared shader (no emissive surfaces there yet) */
    unsigned int tex_velocity;     /* RG16F: real camera-motion UV-space delta, see renderer.c's u_prev_mvp */
    unsigned int tex_object_id;    /* R32UI: per-pixel object id, see gbuffer_pick_object_id */
    unsigned int tex_depth_stencil;/* DEPTH24_STENCIL8, samplable */

    unsigned int hdr_fbo;
    unsigned int hdr_tex;          /* RGBA16F accumulation target the lighting pass writes */

    /* Shadow map: depth-only render from a fixed directional light's
     * point of view. Fixed, hand-picked ortho volume sized to cover the
     * default arena footprint (see gbuffer.c) — doesn't fit itself to
     * what's actually visible/built, so geometry far outside that (via
     * the editor) won't be shadowed correctly. A scene-fitted or
     * cascaded shadow map is future work, this is a first pass. */
    unsigned int shadow_fbo;
    unsigned int shadow_tex;       /* DEPTH_COMPONENT32F, samplable */
    int          shadow_size;
    unsigned int shadow_program;
    int          shadow_u_light_vp;
    float        light_vp[16];     /* set by gbuffer_render_shadow_map, read by gbuffer_resolve */

    /* Intermediate LDR target tonemap writes to, instead of the default
     * framebuffer directly — needed so fxaa has something to read before
     * the frame actually gets presented. */
    unsigned int ldr_fbo;
    unsigned int ldr_tex;           /* RGBA8 */

    /* Bloom: threshold-extract (tex_bright) then a same-resolution 2-pass
     * separable blur (tex_blur_a = horizontal pass, tex_blur_b = vertical
     * pass = final blurred result), additively composited back into
     * hdr_tex before tonemap. Honest caveat: MOST current game content
     * (world/ground/players/rockets) still never exceeds ~1.0 HDR — no
     * emissive materials, no over-bright lights on that shared shader path
     * — but a MeshObject face with a high emission value (see halfedge.h's
     * per-face material, settable via the console's matemit command) now
     * genuinely can, and will genuinely bloom; the standard >1.0 threshold
     * was already correct, working plumbing, it just had nothing to catch
     * before this. Not a full mip-chain
     * downsample/upsample (real bloom implementations usually use one
     * for a softer falloff) — same-resolution 2-pass blur is enough for
     * a first pass. */
    unsigned int bright_fbo,  tex_bright;   /* RGBA16F */
    unsigned int blur_fbo_a,  tex_blur_a;   /* RGBA16F, horizontal blur result */
    unsigned int blur_fbo_b,  tex_blur_b;   /* RGBA16F, vertical blur result = final bloom */

    /* Transparency: a fixed NDC-space test quad drawn forward-blended into
     * hdr_fbo, depth-tested (not depth-writing) against tex_depth_stencil
     * (shared with the opaque G-buffer's fbo, see gbuffer_create). See
     * gbuffer.c's TRANSPARENT_TEST_VERT_SRC comment for why it's a fixed
     * on-screen probe rather than real world content — nothing
     * transparent exists in the game yet to exercise this path with. */
    unsigned int transparent_test_vbo, transparent_test_program;
    int transparent_test_u_color;
    /* A tiny (1x1) offscreen target the one-shot blend-math self-check in
     * gbuffer_resolve draws into instead of the real hdr_fbo -- see that
     * function's own comment on why: this used to draw into hdr_fbo
     * directly, which put a real, visible translucent red probe quad over
     * the actual rendered frame for one frame, in every build (editor,
     * player, wasm/browser alike), forever. Same RGBA16F format as
     * hdr_tex so the blend math it exercises is bit-for-bit the same
     * precision as the real pipeline. */
    unsigned int transparent_test_scratch_fbo, transparent_test_scratch_tex;

    /* TAA: temporal resolve between tonemap and fxaa. taa_tex_a/b are a
     * ping-pong pair (see taa_write_idx below) — each frame writes the
     * blended result into one and reads the OTHER (last frame's result) as
     * history, then swaps for next frame. 4-tap cross neighborhood-AABB
     * clamping (a cheaper, recognized simpler variant of the standard
     * technique) guards against ghosting when reprojecting history via
     * tex_velocity. Honest caveat, same one as tex_velocity above: velocity
     * only captures camera motion, not each object's own movement, so
     * fast-moving players/rockets will show mild ghosting/blur that a full
     * per-object-motion implementation wouldn't have — a correct, scoped-
     * down first pass, not the finished thing, matching the shadow map's
     * static-geometry-only precedent. Whether it's actually ghosting-free
     * in practice for the camera-motion case is a visual judgment call
     * that can't be fully proven headlessly — the mechanical parts
     * (velocity values, history read/write, blend math) are what's
     * numerically verified here. */
    unsigned int taa_fbo_a, taa_tex_a;
    unsigned int taa_fbo_b, taa_tex_b;
    int          taa_write_idx;     /* 0 -> write taa_tex_a/read taa_tex_b this frame, 1 -> the reverse */
    int          taa_history_valid; /* 0 on the very first gbuffer_resolve call (no history yet) */
    unsigned int taa_program;
    int taa_u_current, taa_u_history, taa_u_velocity, taa_u_texel_size, taa_u_history_valid;

    unsigned int quad_vao, quad_vbo;
    unsigned int lighting_program, tonemap_program, fxaa_program;
    unsigned int brightpass_program, blur_program, composite_program;
    int light_u_albedo, light_u_normal, light_u_depth, light_u_light_dir, light_u_sky_color;
    int light_u_inv_view_proj, light_u_light_vp, light_u_shadow_map;
    int light_u_material, light_u_emissive, light_u_cam_pos;
    /* Real point lights (see gbuffer_set_point_lights) -- summed into the
     * lighting pass alongside the single directional u_light_dir "sun"
     * above, with real inverse-square-ish falloff (LIGHTING_FRAG_SRC).
     * Set once per frame by whoever calls gbuffer_resolve (player_main.c/
     * editor_main.c), read back out here at upload time. */
    int light_u_point_light_count, light_u_point_light_pos, light_u_point_light_color;
    Vec3f point_light_pos[GBUF_MAX_POINT_LIGHTS];
    Vec3f point_light_color[GBUF_MAX_POINT_LIGHTS];   /* pre-multiplied by the light's own energy -- see gbuffer_set_point_lights */
    int   point_light_count;
    int tonemap_u_hdr;
    int fxaa_u_tex, fxaa_u_resolution;
    int bright_u_tex, bright_u_threshold;
    int blur_u_tex, blur_u_texel_size, blur_u_dir;
    int composite_u_tex;
} GBuffer;

GBuffer *gbuffer_create(int w, int h);
void     gbuffer_destroy(GBuffer *gb);
void     gbuffer_resize(GBuffer *gb, int w, int h);

/* Sets where gbuffer_resolve's final FXAA blit lands in the actual window
 * (default 0,0 — bottom-left origin, GL's normal convention, matching a
 * full-window fill). Combined with gbuffer_resize(gb, panel_w, panel_h),
 * this hosts the 3D scene inside an arbitrary sub-rectangle of the window
 * instead of always filling it — what the UI system's Scene panel needs.
 * Takes effect on the next gbuffer_resolve() call, not retroactively. */
void gbuffer_set_viewport_offset(GBuffer *gb, int x, int y);

/* Binds the G-buffer FBO and clears it (depth to far, object_id to the
 * 0xFFFFFFFF "no object" sentinel — color attachments are left as-is:
 * the lighting pass detects untouched/background pixels via depth and
 * outputs sky_color directly without reading them, so clearing them has
 * no observable effect and is skipped). Every renderer_draw_* call after
 * this writes into the G-buffer exactly as it used to write to the
 * default framebuffer. */
void gbuffer_begin_geometry_pass(GBuffer *gb, const float *sky_color);

/* Renders `mesh` (world geometry only — see the struct comment on the
 * scope limitation) depth-only into the shadow map from light_dir's point
 * of view. Call after the geometry pass, before gbuffer_resolve. */
void gbuffer_render_shadow_map(GBuffer *gb, RenderMesh *mesh, const float *light_dir);

/* Lighting pass (G-buffer -> HDR, shadow-mapped), bloom (threshold+blur,
 * additively composited back into HDR), transparency (forward-blended test
 * quad, depth-tested but not depth-writing, into HDR), tonemap pass
 * (HDR -> intermediate LDR texture), TAA (temporal resolve against
 * history, velocity-reprojected and neighborhood-clamped), then FXAA
 * (-> default framebuffer, standard luma-edge-detection formulation).
 * Call once per frame after gbuffer_render_shadow_map. inv_view_proj is
 * the camera's inverse view-projection matrix
 * (renderer_get_inverse_view_proj), needed to reconstruct world-space
 * position from G-buffer depth for shadow-space projection. cam_pos
 * (Renderer.cam_pos) is the camera's world-space position, needed for the
 * lighting pass's specular view direction now that it reads real per-face
 * metallic/roughness (see LIGHTING_FRAG_SRC's own comment). */
void gbuffer_resolve(GBuffer *gb, const float *light_dir, const float *sky_color,
                      const float *inv_view_proj, const float *cam_pos);

/* Sets the real point lights the NEXT gbuffer_resolve call sums into its
 * lighting pass (see LIGHTING_FRAG_SRC's own point-light loop) -- call
 * once per frame, before gbuffer_resolve, same convention gbuffer_render_
 * shadow_map's light_dir argument already has for the single directional
 * "sun". count is clamped to GBUF_MAX_POINT_LIGHTS (extras silently
 * dropped, not an error -- same honest-degradation convention this
 * codebase's other small fixed-capacity registries use, e.g. light.h's
 * own PHI_MAX_LIGHTS). colors should already be pre-multiplied by each
 * light's own energy (this function doesn't know about PhiLight's own
 * struct shape -- see light.h -- so the caller, which does, does that
 * multiply itself; a real, simple point light with a real inverse-
 * square-ish falloff, not a physically-calibrated photometric one). */
void gbuffer_set_point_lights(GBuffer *gb, const Vec3f *positions, const Vec3f *colors, int count);

/* Phase 0's "readPixels object ID selection" deliverable: reads back one
 * texel of the object-id attachment. (x,y) are framebuffer pixels,
 * origin bottom-left (standard GL convention). Returns 0xFFFFFFFF where
 * nothing was drawn. */
unsigned int gbuffer_pick_object_id(GBuffer *gb, int x, int y);
