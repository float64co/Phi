#include "gbuffer.h"
#include <GL/gl.h>
#include "gl_native.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_pos * 0.5 + 0.5;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *LIGHTING_FRAG_SRC =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_albedo;\n"
    "uniform sampler2D u_normal;\n"
    "uniform sampler2D u_depth;\n"
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
    "  out_hdr = vec4(albedo * (ambient + diff * 0.7), 1.0);\n"
    "}\n";

static const char *TONEMAP_FRAG_SRC =
    "#version 330 core\n"
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
    gb->light_u_albedo    = glGetUniformLocation(gb->lighting_program, "u_albedo");
    gb->light_u_normal    = glGetUniformLocation(gb->lighting_program, "u_normal");
    gb->light_u_depth     = glGetUniformLocation(gb->lighting_program, "u_depth");
    gb->light_u_light_dir = glGetUniformLocation(gb->lighting_program, "u_light_dir");
    gb->light_u_sky_color = glGetUniformLocation(gb->lighting_program, "u_sky_color");

    gb->tonemap_program = link(QUAD_VERT_SRC, TONEMAP_FRAG_SRC);
    gb->tonemap_u_hdr = glGetUniformLocation(gb->tonemap_program, "u_hdr");

    printf("[gbuffer] created %dx%d, lighting_prog=%u tonemap_prog=%u\n",
           w, h, gb->lighting_program, gb->tonemap_program);
    gl_check("gbuffer_create");
    return gb;
}

static void free_gl_resources(GBuffer *gb) {
    unsigned int texs[] = { gb->tex_albedo, gb->tex_normal, gb->tex_material, gb->tex_emissive,
                             gb->tex_velocity, gb->tex_object_id, gb->tex_depth_stencil, gb->hdr_tex };
    glDeleteTextures((int)(sizeof(texs) / sizeof(texs[0])), texs);
    glDeleteFramebuffers(1, &gb->fbo);
    glDeleteFramebuffers(1, &gb->hdr_fbo);
    glDeleteProgram(gb->lighting_program);
    glDeleteProgram(gb->tonemap_program);
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

void gbuffer_resolve(GBuffer *gb, const float *light_dir, const float *sky_color) {
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
    glUniform3fv(gb->light_u_light_dir, 1, light_dir);
    glUniform3fv(gb->light_u_sky_color, 1, sky_color);
    glBindVertexArray(gb->quad_vao);
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
