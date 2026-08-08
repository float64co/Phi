#include "gbuffer.h"
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>   /* not GLES2/gl2.h — MRT/FBO/integer-texture support (GLES2/WebGL1 had none of it) */
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* GLSL ES 3.00 (wasm/WebGL2) still needs a precision qualifier, unlike
 * desktop GLSL 330 core — see renderer.c's shader comment for the same
 * split. Applied uniformly to every shader stage here (harmless on a
 * vertex shader, which doesn't strictly require one) to keep one shared
 * version-line macro instead of separate vertex/fragment variants. */
#ifdef __EMSCRIPTEN__
#define GBUF_SHADER_HEADER "#version 300 es\nprecision mediump float;\n"
#else
#define GBUF_SHADER_HEADER "#version 330 core\n"
#endif

/* ---- Small matrix helpers for the shadow pass's light-space camera.
 * Deliberately not shared with renderer.c (kept self-contained, same
 * reasoning as the shader-compile helpers below) except for the
 * cross-product convention, which is copied exactly from
 * renderer.c's mat4_look_rotation/mat4_look_dir — that convention was
 * fixed once already after a real handedness bug there (see its
 * comments), so this reuses it rather than re-deriving and risking the
 * same mistake twice. ---- */

static void mat4_mul(float *out, const float *a, const float *b) {
    float tmp[16];
    for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a[k*4+row] * b[col*4+k];
        tmp[col*4+row] = s;
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_ortho(float *m, float left, float right, float bottom, float top,
                        float near_, float far_) {
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / (right - left);
    m[5]  = 2.0f / (top - bottom);
    m[10] = -2.0f / (far_ - near_);
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[14] = -(far_ + near_) / (far_ - near_);
    m[15] = 1.0f;
}

static void mat4_look_at(float *m, float ex, float ey, float ez,
                          float tx, float ty, float tz) {
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= flen; fy /= flen; fz /= flen;

    float upx = 0.0f, upy = 1.0f, upz = 0.0f;
    if (fabsf(fx*upx + fy*upy + fz*upz) > 0.999f) { upx = 1.0f; upy = 0.0f; upz = 0.0f; }

    float rx = fy*upz - fz*upy, ry = fz*upx - fx*upz, rz = fx*upy - fy*upx;
    float rl = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= rl; ry /= rl; rz /= rl;

    float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;

    m[0] = rx;  m[4] = ry;  m[8]  = rz;  m[12] = -(rx*ex + ry*ey + rz*ez);
    m[1] = ux;  m[5] = uy;  m[9]  = uz;  m[13] = -(ux*ex + uy*ey + uz*ez);
    m[2] = -fx; m[6] = -fy; m[10] = -fz; m[14] =  (fx*ex + fy*ey + fz*ez);
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

static void gl_check(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR)
        printf("[gbuffer] GL error 0x%04x at %s\n", e, where);
}

static unsigned int make_target(int w, int h, GLint internal, GLenum format, GLenum type) {
    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

/* ---- Tiny shader helpers (deliberately not shared with renderer.c's
 * static compile_shader/link_program — this is a separate translation
 * unit and the passes here are simple enough that duplicating ~15 lines
 * beats coupling the two files together for it). ---- */
static unsigned int compile(GLenum type, const char *src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
        printf("[gbuffer] shader compile error: %s\n", log);
    }
    return s;
}

static unsigned int link(const char *vsrc, const char *fsrc) {
    unsigned int vs = compile(GL_VERTEX_SHADER, vsrc);
    unsigned int fs = compile(GL_FRAGMENT_SHADER, fsrc);
    unsigned int p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, NULL, log);
        printf("[gbuffer] program link error: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

static const char *QUAD_VERT_SRC =
    GBUF_SHADER_HEADER
    "layout(location=0) in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_pos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *LIGHTING_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_albedo;\n"
    "uniform sampler2D u_normal;\n"
    "uniform sampler2D u_depth;\n"
    "uniform sampler2D u_shadow_map;\n"
    "uniform mat4 u_inv_view_proj;\n"
    "uniform mat4 u_light_vp;\n"
    "uniform vec3 u_light_dir;\n"
    "uniform vec3 u_sky_color;\n"
    "out vec4 out_hdr;\n"
    "void main() {\n"
    /* Untouched/background pixels read back the far-plane depth this
     * G-buffer was cleared to — nothing was drawn there, so skip lighting
     * entirely and show the sky color directly rather than lighting
     * whatever garbage happens to be in albedo/normal at that pixel. */
    "  float depth = texture(u_depth, v_uv).r;\n"
    "  if (depth >= 0.999999) { out_hdr = vec4(u_sky_color, 1.0); return; }\n"
    "  vec3 albedo = texture(u_albedo, v_uv).rgb;\n"
    "  vec3 n = texture(u_normal, v_uv).rgb * 2.0 - 1.0;\n"
    "  float diff = max(dot(n, u_light_dir), 0.0);\n"
    "  float ambient = 0.3;\n"
    /* Reconstruct world-space position from this pixel's UV + depth via
     * the camera's inverse view-projection (both stored in [0,1]/depth-
     * buffer ranges, remapped to NDC's [-1,1] before unprojecting). */
    "  vec4 clip = vec4(v_uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);\n"
    "  vec4 world = u_inv_view_proj * clip;\n"
    "  world /= world.w;\n"
    /* Project into the shadow map's light space to look up occluder depth. */
    "  vec4 lclip = u_light_vp * world;\n"
    "  vec3 lndc = lclip.xyz / lclip.w;\n"
    "  vec3 lsc = lndc * 0.5 + 0.5;\n"
    "  float shadow = 1.0;\n"
    "  if (lsc.x >= 0.0 && lsc.x <= 1.0 && lsc.y >= 0.0 && lsc.y <= 1.0 && lsc.z <= 1.0) {\n"
    "    float occluder_depth = texture(u_shadow_map, lsc.xy).r;\n"
    "    float bias = 0.002;\n"   /* tuned to avoid acne on this scene's scale; may need revisiting once seen visually */
    "    if (lsc.z - bias > occluder_depth) shadow = 0.3;\n"  /* in shadow: dim, not black — crude, no PCF/soft edges yet */
    "  }\n"
    "  out_hdr = vec4(albedo * (ambient + diff * 0.7 * shadow), 1.0);\n"
    "}\n";

static const char *SHADOW_VERT_SRC =
    GBUF_SHADER_HEADER
    "layout(location=0) in vec3 a_pos;\n"
    "uniform mat4 u_light_vp;\n"
    "void main() {\n"
    "  gl_Position = u_light_vp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *SHADOW_FRAG_SRC =
    GBUF_SHADER_HEADER
    "void main() {\n"
    "}\n";

static const char *TONEMAP_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_hdr;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    /* Passthrough for now — current content never exceeds [0,1] (no
     * bloom/emissive/over-bright lights yet), so a real Reinhard/ACES
     * curve would only darken the image with nothing to compress.
     * Swap this for real tonemapping once HDR content exists. */
    "  out_color = vec4(texture(u_hdr, v_uv).rgb, 1.0);\n"
    "}\n";

GBuffer *gbuffer_create(int w, int h) {
    GBuffer *gb = (GBuffer *)calloc(1, sizeof(GBuffer));
    gb->w = w; gb->h = h;

    gb->tex_albedo        = make_target(w, h, GL_RGBA8,        GL_RGBA,         GL_UNSIGNED_BYTE);
    gb->tex_normal        = make_target(w, h, GL_RGB10_A2,     GL_RGBA,         GL_UNSIGNED_INT_2_10_10_10_REV);
    gb->tex_material      = make_target(w, h, GL_RGBA8,        GL_RGBA,         GL_UNSIGNED_BYTE);
    gb->tex_emissive      = make_target(w, h, GL_R11F_G11F_B10F, GL_RGB,        GL_UNSIGNED_INT_10F_11F_11F_REV);
    gb->tex_velocity      = make_target(w, h, GL_RG16F,        GL_RG,           GL_FLOAT);
    gb->tex_object_id     = make_target(w, h, GL_R32UI,        GL_RED_INTEGER,  GL_UNSIGNED_INT);
    gb->tex_depth_stencil = make_target(w, h, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);

    glGenFramebuffers(1, &gb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->tex_albedo, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gb->tex_normal, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gb->tex_material, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, gb->tex_emissive, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT4, GL_TEXTURE_2D, gb->tex_velocity, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT5, GL_TEXTURE_2D, gb->tex_object_id, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gb->tex_depth_stencil, 0);
    GLenum bufs[6] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2,
                       GL_COLOR_ATTACHMENT3, GL_COLOR_ATTACHMENT4, GL_COLOR_ATTACHMENT5 };
    glDrawBuffers(6, bufs);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] G-buffer FBO incomplete: 0x%04x\n", status);

    gb->hdr_tex = make_target(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glGenFramebuffers(1, &gb->hdr_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->hdr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->hdr_tex, 0);
    GLenum hdr_buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &hdr_buf);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] HDR FBO incomplete: 0x%04x\n", status);

    /* Shadow map: depth-only FBO, no color attachment at all. GL_NONE via
     * glDrawBuffers(1,&none) tells GL not to expect one — glDrawBuffer
     * (singular) would do the same on desktop GL, but doesn't exist in
     * GLES3/WebGL2 at all (only the array-based plural form does), so
     * this is the one spelling that's portable to both. */
    gb->shadow_size = 2048;
    gb->shadow_tex = make_target(gb->shadow_size, gb->shadow_size,
                                  GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT);
    glGenFramebuffers(1, &gb->shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gb->shadow_tex, 0);
    { GLenum none = GL_NONE; glDrawBuffers(1, &none); }
    glReadBuffer(GL_NONE);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] shadow FBO incomplete: 0x%04x\n", status);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    float quad[] = { -1,-1,  1,-1,  1,1,   -1,-1,  1,1,  -1,1 };
    glGenVertexArrays(1, &gb->quad_vao);
    glBindVertexArray(gb->quad_vao);
    glGenBuffers(1, &gb->quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gb->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

    gb->lighting_program = link(QUAD_VERT_SRC, LIGHTING_FRAG_SRC);
    gb->light_u_albedo        = glGetUniformLocation(gb->lighting_program, "u_albedo");
    gb->light_u_normal        = glGetUniformLocation(gb->lighting_program, "u_normal");
    gb->light_u_depth         = glGetUniformLocation(gb->lighting_program, "u_depth");
    gb->light_u_light_dir     = glGetUniformLocation(gb->lighting_program, "u_light_dir");
    gb->light_u_sky_color     = glGetUniformLocation(gb->lighting_program, "u_sky_color");
    gb->light_u_inv_view_proj = glGetUniformLocation(gb->lighting_program, "u_inv_view_proj");
    gb->light_u_light_vp      = glGetUniformLocation(gb->lighting_program, "u_light_vp");
    gb->light_u_shadow_map    = glGetUniformLocation(gb->lighting_program, "u_shadow_map");

    gb->tonemap_program = link(QUAD_VERT_SRC, TONEMAP_FRAG_SRC);
    gb->tonemap_u_hdr = glGetUniformLocation(gb->tonemap_program, "u_hdr");

    gb->shadow_program = link(SHADOW_VERT_SRC, SHADOW_FRAG_SRC);
    gb->shadow_u_light_vp = glGetUniformLocation(gb->shadow_program, "u_light_vp");

    printf("[gbuffer] created %dx%d, lighting_prog=%u tonemap_prog=%u shadow_prog=%u (%dx%d)\n",
           w, h, gb->lighting_program, gb->tonemap_program, gb->shadow_program,
           gb->shadow_size, gb->shadow_size);
    gl_check("gbuffer_create");
    return gb;
}

static void free_gl_resources(GBuffer *gb) {
    unsigned int texs[] = { gb->tex_albedo, gb->tex_normal, gb->tex_material, gb->tex_emissive,
                             gb->tex_velocity, gb->tex_object_id, gb->tex_depth_stencil, gb->hdr_tex,
                             gb->shadow_tex };
    glDeleteTextures((int)(sizeof(texs) / sizeof(texs[0])), texs);
    glDeleteFramebuffers(1, &gb->fbo);
    glDeleteFramebuffers(1, &gb->hdr_fbo);
    glDeleteFramebuffers(1, &gb->shadow_fbo);
    glDeleteProgram(gb->lighting_program);
    glDeleteProgram(gb->tonemap_program);
    glDeleteProgram(gb->shadow_program);
}

void gbuffer_destroy(GBuffer *gb) {
    if (!gb) return;
    free_gl_resources(gb);
    free(gb);
}

void gbuffer_resize(GBuffer *gb, int w, int h) {
    if (w == gb->w && h == gb->h) return;
    /* Simplest correct approach: tear down and recreate at the new size
     * rather than resizing textures in place. Resizes are rare (window
     * resize events only), so the extra allocation churn doesn't matter.
     * free_gl_resources (not gbuffer_destroy) so *gb keeps its identity —
     * main.c holds a GBuffer* across resizes and shouldn't have to know
     * this reallocated anything. */
    free_gl_resources(gb);
    GBuffer *fresh = gbuffer_create(w, h);
    *gb = *fresh;
    free(fresh);
}

void gbuffer_begin_geometry_pass(GBuffer *gb, const float *sky_color) {
    (void)sky_color;  /* background is handled in the lighting pass via depth, not by clearing color attachments here */
    glBindFramebuffer(GL_FRAMEBUFFER, gb->fbo);
    glViewport(0, 0, gb->w, gb->h);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    unsigned int no_object = 0xFFFFFFFFu;
    glClearBufferuiv(GL_COLOR, 5, &no_object);  /* draw-buffer index 5 = GL_COLOR_ATTACHMENT5 = object_id */
}

void gbuffer_render_shadow_map(GBuffer *gb, RenderMesh *mesh, const float *light_dir) {
    if (!mesh || mesh->count == 0) return;

    /* Fixed light-space camera: sits back along light_dir from a point
     * roughly in the middle of the default arena (world coords the whole
     * codebase already spawns players at, see main.c) and looks back at
     * it through a fixed ortho volume. See the struct comment in
     * gbuffer.h for the scope limitation (doesn't fit itself to what's
     * actually built). */
    float scene_cx = 128.0f, scene_cy = 32.0f, scene_cz = 128.0f;
    float dist = 600.0f;
    float ex = scene_cx + light_dir[0] * dist;
    float ey = scene_cy + light_dir[1] * dist;
    float ez = scene_cz + light_dir[2] * dist;

    float light_view[16], light_proj[16];
    mat4_look_at(light_view, ex, ey, ez, scene_cx, scene_cy, scene_cz);
    mat4_ortho(light_proj, -350.0f, 350.0f, -350.0f, 350.0f, 10.0f, 1200.0f);
    mat4_mul(gb->light_vp, light_proj, light_view);

    glBindFramebuffer(GL_FRAMEBUFFER, gb->shadow_fbo);
    glViewport(0, 0, gb->shadow_size, gb->shadow_size);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glUseProgram(gb->shadow_program);
    glUniformMatrix4fv(gb->shadow_u_light_vp, 1, GL_FALSE, gb->light_vp);

    /* Only need position (location 0) — reuse the quad VAO's binding
     * slot, its own attribs get fully overridden below before drawing. */
    glBindVertexArray(gb->quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, VERTEX_STRIDE * (int)sizeof(float), (void *)0);
    glDrawArrays(GL_TRIANGLES, 0, mesh->count);
    gl_check("gbuffer_render_shadow_map");

#ifndef __EMSCRIPTEN__
    /* One-shot sanity check: sample a handful of shadow-map texels and
     * confirm they actually vary — a degenerate render (nothing rasterized,
     * or a broken light matrix putting everything outside the frustum)
     * would read back as a uniform 1.0 (cleared-and-never-written) across
     * every sample instead. Native only: readPixels(GL_DEPTH_COMPONENT,
     * GL_FLOAT) against a depth-only FBO isn't a legal format/type
     * combination under WebGL2/ANGLE (confirmed — it raises
     * INVALID_ENUM there and returns garbage), unlike desktop GL where
     * it's fine. No portable equivalent attempted here; the shadow map's
     * correctness was already independently verified on native (Linux
     * and Windows, identical depth values on both), which is the
     * evidence this diagnostic exists to produce in the first place. */
    static int s_checked = 0;
    if (!s_checked) {
        s_checked = 1;
        int s = gb->shadow_size;
        int px[5][2] = { {s/2,s/2}, {s/4,s/4}, {3*s/4,s/4}, {s/4,3*s/4}, {3*s/4,3*s/4} };
        float depths[5];
        for (int i = 0; i < 5; i++)
            glReadPixels(px[i][0], px[i][1], 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depths[i]);
        printf("[gbuffer] shadow map sample depths (center,4 quadrants): "
               "%.4f %.4f %.4f %.4f %.4f\n",
               depths[0], depths[1], depths[2], depths[3], depths[4]);
    }
#endif

    /* Restore the quad's own attrib binding for the lighting/tonemap
     * passes that follow — same VAO, different vertex data. */
    glBindBuffer(GL_ARRAY_BUFFER, gb->quad_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
}

void gbuffer_resolve(GBuffer *gb, const float *light_dir, const float *sky_color,
                      const float *inv_view_proj) {
    /* ---- Lighting: G-buffer -> HDR ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, gb->hdr_fbo);
    glViewport(0, 0, gb->w, gb->h);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(gb->lighting_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->tex_albedo);
    glUniform1i(gb->light_u_albedo, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gb->tex_normal);
    glUniform1i(gb->light_u_normal, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gb->tex_depth_stencil);
    glUniform1i(gb->light_u_depth, 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, gb->shadow_tex);
    glUniform1i(gb->light_u_shadow_map, 3);
    glUniform3fv(gb->light_u_light_dir, 1, light_dir);
    glUniform3fv(gb->light_u_sky_color, 1, sky_color);
    glUniformMatrix4fv(gb->light_u_inv_view_proj, 1, GL_FALSE, inv_view_proj);
    glUniformMatrix4fv(gb->light_u_light_vp, 1, GL_FALSE, gb->light_vp);
    glBindVertexArray(gb->quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, gb->quad_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/lighting");

    /* ---- Tonemap: HDR -> default framebuffer ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gb->w, gb->h);
    glUseProgram(gb->tonemap_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->hdr_tex);
    glUniform1i(gb->tonemap_u_hdr, 0);
    glBindVertexArray(gb->quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/tonemap");

    glEnable(GL_DEPTH_TEST);
}

unsigned int gbuffer_pick_object_id(GBuffer *gb, int x, int y) {
    glBindFramebuffer(GL_FRAMEBUFFER, gb->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT5);
    unsigned int id = 0xFFFFFFFFu;
    glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return id;
}
