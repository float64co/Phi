#pragma once
/* Deferred renderer G-buffer (Phase 0, native only — see phi.md's
 * "Deferred Renderer and G-Buffer" section for the target layout this
 * implements). WebGL1/GLES2 (the current wasm target) can't do multiple
 * render targets, float textures, or integer textures, so wasm keeps
 * rendering forward, straight to the default framebuffer, completely
 * unaffected by this file — it's only ever compiled into the native build.
 *
 * Three stages, matching the pipeline diagram in phi.md:
 *   [geometry] -> writes this G-buffer            (gbuffer_begin_geometry_pass,
 *                                                    then the existing
 *                                                    renderer_draw_* calls)
 *   [lighting] -> HDR accumulation (RGBA16F)       (gbuffer_resolve, part 1)
 *   [tonemap]  -> LDR to the default framebuffer   (gbuffer_resolve, part 2)
 *
 * Not implemented yet, left for later phases/passes: shadow maps,
 * transparency, TAA, bloom, FXAA, and the Python @phi.render_pass
 * insertion-point system — this is the minimum deferred pipeline the rest
 * of that can be built on, not the finished thing. */

typedef struct {
    int w, h;

    unsigned int fbo;
    unsigned int tex_albedo;       /* RGBA8: base color (RGB) + AO (A, unused yet, always 1.0) */
    unsigned int tex_normal;       /* RGB10_A2: world normal *0.5+0.5 (RGB) + metallic (A, unused yet) */
    unsigned int tex_material;     /* RGBA8: roughness/emissive-mask/object-tag/spare — allocated, not yet meaningfully populated */
    unsigned int tex_emissive;     /* R11F_G11F_B10F: allocated, always black — no emissive surfaces yet */
    unsigned int tex_velocity;     /* RG16F: allocated, always zero — no motion-vector tracking yet */
    unsigned int tex_object_id;    /* R32UI: per-pixel object id, see gbuffer_pick_object_id */
    unsigned int tex_depth_stencil;/* DEPTH24_STENCIL8, samplable */

    unsigned int hdr_fbo;
    unsigned int hdr_tex;          /* RGBA16F accumulation target the lighting pass writes */

    unsigned int quad_vao, quad_vbo;
    unsigned int lighting_program, tonemap_program;
    int light_u_albedo, light_u_normal, light_u_depth, light_u_light_dir, light_u_sky_color;
    int tonemap_u_hdr;
} GBuffer;

GBuffer *gbuffer_create(int w, int h);
void     gbuffer_destroy(GBuffer *gb);
void     gbuffer_resize(GBuffer *gb, int w, int h);

/* Binds the G-buffer FBO and clears it (depth to far, object_id to the
 * 0xFFFFFFFF "no object" sentinel — color attachments are left as-is:
 * the lighting pass detects untouched/background pixels via depth and
 * outputs sky_color directly without reading them, so clearing them has
 * no observable effect and is skipped). Every renderer_draw_* call after
 * this writes into the G-buffer exactly as it used to write to the
 * default framebuffer. */
void gbuffer_begin_geometry_pass(GBuffer *gb, const float *sky_color);

/* Lighting pass (G-buffer -> HDR) then tonemap pass (HDR -> default
 * framebuffer). Call once per frame after all geometry is drawn. */
void gbuffer_resolve(GBuffer *gb, const float *light_dir, const float *sky_color);

/* Phase 0's "readPixels object ID selection" deliverable: reads back one
 * texel of the object-id attachment. (x,y) are framebuffer pixels,
 * origin bottom-left (standard GL convention). Returns 0xFFFFFFFF where
 * nothing was drawn. */
unsigned int gbuffer_pick_object_id(GBuffer *gb, int x, int y);
