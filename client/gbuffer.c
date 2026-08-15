#include "gbuffer.h"
#include "render_hooks.h"
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

/* Lighting pass -- reads the FULL G-buffer now, not just albedo/normal:
 * out_material (metallic/roughness) and out_emissive were being written by
 * the geometry pass all along but never sampled here, so every surface's
 * real material data was silently discarded and every object looked
 * flat-diffuse regardless of what it actually was (see phi.md's PBR
 * material note). This is a deliberately simplified Blinn-Phong-with-PBR-
 * inputs approximation, not a full Cook-Torrance/GGX BRDF -- real
 * metallic/roughness/emission now genuinely change the lit result (a
 * mirror-smooth metal face visibly differs from a rough dielectric one),
 * which is the actual bar for "real PBR inputs" this phase needs, without
 * a large new BRDF implementation it doesn't. */
static const char *LIGHTING_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_albedo;\n"
    "uniform sampler2D u_normal;\n"
    "uniform sampler2D u_depth;\n"
    "uniform sampler2D u_material;\n"
    "uniform sampler2D u_emissive;\n"
    "uniform sampler2D u_shadow_map;\n"
    "uniform mat4 u_inv_view_proj;\n"
    "uniform mat4 u_light_vp;\n"
    "uniform vec3 u_light_dir;\n"
    "uniform vec3 u_sky_color;\n"
    "uniform vec3 u_cam_pos;\n"
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
    "  vec2 mat = texture(u_material, v_uv).rg;\n"
    "  float metallic = mat.x;\n"
    "  float roughness = clamp(mat.y, 0.05, 1.0);\n"   /* floor avoids a divide-by-zero-ish infinite-shininess highlight at roughness==0 */
    "  vec3 emissive = texture(u_emissive, v_uv).rgb;\n"
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
    /* Metallic surfaces have ~no separate diffuse albedo in a physically-
     * based model (their reflectance is all specular, tinted by their own
     * color instead of white) — diffuse_albedo fades toward 0 as metallic
     * rises, f0 (specular reflectance at normal incidence) blends from a
     * flat 0.04 dielectric baseline toward the surface's own albedo. */
    "  vec3 view_dir = normalize(u_cam_pos - world.xyz);\n"
    "  vec3 half_dir = normalize(u_light_dir + view_dir);\n"
    "  float ndoth = max(dot(n, half_dir), 0.0);\n"
    "  float shininess = mix(4.0, 128.0, pow(1.0 - roughness, 2.0));\n"
    "  vec3 f0 = mix(vec3(0.04), albedo, metallic);\n"
    "  vec3 specular = f0 * pow(ndoth, shininess) * (1.0 - roughness * 0.9) * shadow;\n"
    "  vec3 diffuse_albedo = albedo * (1.0 - metallic);\n"
    "  vec3 lit = diffuse_albedo * (ambient + diff * 0.7 * shadow) + specular + emissive;\n"
    "  out_hdr = vec4(lit, 1.0);\n"
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

/* Standard luma-edge-detection FXAA — the widely-circulated simplified
 * form derived from NVIDIA's original FXAA whitepaper (the same
 * structure appears across many open-source engines/shader collections
 * under this name), not a from-scratch reimplementation. Detects a local
 * contrast direction from the 4 diagonal neighbors' luma, blends along
 * it, and rejects the blend (falls back to the 2-tap result) if it moves
 * luma outside the local min/max — the usual FXAA correctness guard
 * against over-blurring. */
static const char *FXAA_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_resolution;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  vec2 invRes = 1.0 / u_resolution;\n"
    "  float SPAN_MAX = 8.0;\n"
    "  float REDUCE_MUL = 1.0 / 8.0;\n"
    "  float REDUCE_MIN = 1.0 / 128.0;\n"
    "  vec3 rgbNW = texture(u_tex, v_uv + vec2(-1.0,-1.0) * invRes).rgb;\n"
    "  vec3 rgbNE = texture(u_tex, v_uv + vec2( 1.0,-1.0) * invRes).rgb;\n"
    "  vec3 rgbSW = texture(u_tex, v_uv + vec2(-1.0, 1.0) * invRes).rgb;\n"
    "  vec3 rgbSE = texture(u_tex, v_uv + vec2( 1.0, 1.0) * invRes).rgb;\n"
    "  vec3 rgbM  = texture(u_tex, v_uv).rgb;\n"
    "  vec3 lumaW = vec3(0.299, 0.587, 0.114);\n"
    "  float lumaNW = dot(rgbNW, lumaW);\n"
    "  float lumaNE = dot(rgbNE, lumaW);\n"
    "  float lumaSW = dot(rgbSW, lumaW);\n"
    "  float lumaSE = dot(rgbSE, lumaW);\n"
    "  float lumaM  = dot(rgbM,  lumaW);\n"
    "  float lumaMin = min(lumaM, min(min(lumaNW,lumaNE), min(lumaSW,lumaSE)));\n"
    "  float lumaMax = max(lumaM, max(max(lumaNW,lumaNE), max(lumaSW,lumaSE)));\n"
    "  vec2 dir;\n"
    "  dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));\n"
    "  dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));\n"
    "  float dirReduce = max((lumaNW+lumaNE+lumaSW+lumaSE) * (0.25*REDUCE_MUL), REDUCE_MIN);\n"
    "  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
    "  dir = clamp(dir * rcpDirMin, vec2(-SPAN_MAX), vec2(SPAN_MAX)) * invRes;\n"
    "  vec3 rgbA = 0.5 * (\n"
    "      texture(u_tex, v_uv + dir * (1.0/3.0 - 0.5)).rgb +\n"
    "      texture(u_tex, v_uv + dir * (2.0/3.0 - 0.5)).rgb);\n"
    "  vec3 rgbB = rgbA * 0.5 + 0.25 * (\n"
    "      texture(u_tex, v_uv + dir * -0.5).rgb +\n"
    "      texture(u_tex, v_uv + dir *  0.5).rgb);\n"
    "  float lumaB = dot(rgbB, lumaW);\n"
    "  if (lumaB < lumaMin || lumaB > lumaMax) out_color = vec4(rgbA, 1.0);\n"
    "  else out_color = vec4(rgbB, 1.0);\n"
    "}\n";

/* ---- Bloom: threshold-extract, then a same-resolution 2-pass separable
 * Gaussian blur (weights are the widely-circulated 9-tap set popularized
 * by LearnOpenGL's bloom tutorial and used across many engines — an
 * established formulation, not derived from scratch), then additive
 * composite back into the HDR buffer. See gbuffer.h's struct comment for
 * the honest "nothing to bloom under current content" caveat. ---- */
static const char *BRIGHTPASS_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_threshold;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  vec3 c = texture(u_tex, v_uv).rgb;\n"
    "  float luma = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  out_color = luma > u_threshold ? vec4(c, 1.0) : vec4(0.0);\n"
    "}\n";

static const char *BLUR_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel_size;\n"
    "uniform vec2 u_dir;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  float w0 = 0.227027;\n"
    "  float w1 = 0.1945946;\n"
    "  float w2 = 0.1216216;\n"
    "  float w3 = 0.054054;\n"
    "  float w4 = 0.016216;\n"
    "  vec3 result = texture(u_tex, v_uv).rgb * w0;\n"
    "  vec2 o1 = u_dir * u_texel_size * 1.0;\n"
    "  vec2 o2 = u_dir * u_texel_size * 2.0;\n"
    "  vec2 o3 = u_dir * u_texel_size * 3.0;\n"
    "  vec2 o4 = u_dir * u_texel_size * 4.0;\n"
    "  result += texture(u_tex, v_uv + o1).rgb * w1 + texture(u_tex, v_uv - o1).rgb * w1;\n"
    "  result += texture(u_tex, v_uv + o2).rgb * w2 + texture(u_tex, v_uv - o2).rgb * w2;\n"
    "  result += texture(u_tex, v_uv + o3).rgb * w3 + texture(u_tex, v_uv - o3).rgb * w3;\n"
    "  result += texture(u_tex, v_uv + o4).rgb * w4 + texture(u_tex, v_uv - o4).rgb * w4;\n"
    "  out_color = vec4(result, 1.0);\n"
    "}\n";

/* Trivial passthrough — used with additive (GL_ONE,GL_ONE) blend state to
 * composite the blurred bright-pass result back into the HDR buffer. */
static const char *COMPOSITE_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  out_color = vec4(texture(u_tex, v_uv).rgb, 1.0);\n"
    "}\n";

/* ---- Transparency test content: a small quad drawn directly in NDC
 * space (no camera transform at all — this isn't world content, just a
 * fixed on-screen probe to exercise the forward-blended pass, per the
 * agreed scope: prove the mechanism works, not add real content). Its
 * depth (-0.8, i.e. depth-buffer value 0.1 under the default
 * glDepthRange(0,1)) is deliberately shallow/near so it passes the depth
 * test against virtually all real scene geometry, while still genuinely
 * exercising the shared-depth-texture test (see hdr_fbo's attachment in
 * gbuffer_create) rather than disabling it. ---- */
static const char *TRANSPARENT_TEST_VERT_SRC =
    GBUF_SHADER_HEADER
    "layout(location=0) in vec2 a_pos;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, -0.8, 1.0);\n"
    "}\n";

static const char *TRANSPARENT_TEST_FRAG_SRC =
    GBUF_SHADER_HEADER
    "uniform vec4 u_color;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  out_color = u_color;\n"
    "}\n";

/* ---- TAA: temporal resolve against a ping-ponged history buffer,
 * reprojected via tex_velocity and neighborhood-AABB-clamped against this
 * frame's own 4-tap cross neighborhood (a cheaper variant of the standard
 * 3x3-neighborhood clamping technique — not invented from scratch, just a
 * cheaper tap pattern). See gbuffer.h's taa_tex_a/b comment for the
 * camera-motion-only velocity caveat. ---- */
static const char *TAA_FRAG_SRC =
    GBUF_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_current;\n"
    "uniform sampler2D u_history;\n"
    "uniform sampler2D u_velocity;\n"
    "uniform vec2 u_texel_size;\n"
    "uniform float u_history_valid;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  vec3 cur = texture(u_current, v_uv).rgb;\n"
    "  vec2 vel = texture(u_velocity, v_uv).rg;\n"
    "  vec2 prev_uv = v_uv - vel;\n"
    "  vec3 n_left  = texture(u_current, v_uv + vec2(-u_texel_size.x, 0.0)).rgb;\n"
    "  vec3 n_right = texture(u_current, v_uv + vec2( u_texel_size.x, 0.0)).rgb;\n"
    "  vec3 n_up    = texture(u_current, v_uv + vec2(0.0,  u_texel_size.y)).rgb;\n"
    "  vec3 n_down  = texture(u_current, v_uv + vec2(0.0, -u_texel_size.y)).rgb;\n"
    "  vec3 nmin = min(cur, min(min(n_left, n_right), min(n_up, n_down)));\n"
    "  vec3 nmax = max(cur, max(max(n_left, n_right), max(n_up, n_down)));\n"
    "  bool off_screen = prev_uv.x < 0.0 || prev_uv.x > 1.0 || prev_uv.y < 0.0 || prev_uv.y > 1.0;\n"
    /* No history yet (first frame ever) or the reprojected sample fell off
     * screen (newly revealed content, e.g. from camera rotation) — fall
     * back to the current frame alone rather than blend with garbage/
     * clamped-to-nothing history. */
    "  if (u_history_valid < 0.5 || off_screen) { out_color = vec4(cur, 1.0); return; }\n"
    "  vec3 hist = texture(u_history, prev_uv).rgb;\n"
    "  hist = clamp(hist, nmin, nmax);\n"
    "  vec3 result = mix(cur, hist, 0.9);\n"  /* 90% history weight — standard strong-accumulation TAA blend factor */
    "  out_color = vec4(result, 1.0);\n"
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
    /* NOT permanently attaching tex_depth_stencil here, even though the
     * transparency pass in gbuffer_resolve needs to depth-test against it
     * while hdr_fbo is bound (a texture CAN be attached to more than one
     * FBO). Tried that first and it broke on WebGL2/ANGLE: the LIGHTING
     * pass also binds hdr_fbo and SAMPLES tex_depth_stencil (as u_depth,
     * for world-position reconstruction) — with it permanently attached,
     * that's a feedback loop (same texture simultaneously bound as the
     * active framebuffer's attachment and as a sampled texture), which
     * desktop GL/Mesa tolerated silently but WebGL2/ANGLE correctly
     * rejects (GL_INVALID_OPERATION, confirmed by the user's browser
     * console: "Feedback loop formed between Framebuffer and active
     * Texture" — this is what silently broke lighting entirely, wasm-only,
     * caught only by real browser testing exactly like the two readPixels
     * bugs earlier in this phase). Fixed by attaching/detaching it around
     * only the transparency draw itself, which doesn't sample this
     * texture (fixed-function depth test only) — see gbuffer_resolve. */
    GLenum hdr_buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &hdr_buf);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] HDR FBO incomplete: 0x%04x\n", status);

    /* Intermediate LDR target: tonemap writes here instead of the default
     * framebuffer directly, so fxaa has a texture to read before the
     * actually-presented frame is produced. */
    gb->ldr_tex = make_target(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &gb->ldr_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->ldr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->ldr_tex, 0);
    GLenum ldr_buf = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &ldr_buf);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] LDR FBO incomplete: 0x%04x\n", status);

    /* Bloom: bright-pass extract + 2-pass separable blur, all at full
     * resolution and RGBA16F (same format as hdr_tex, so the additive
     * composite step has matching precision). */
    gb->tex_bright = make_target(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glGenFramebuffers(1, &gb->bright_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->bright_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->tex_bright, 0);
    { GLenum buf = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &buf); }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] bloom bright-pass FBO incomplete: 0x%04x\n", status);

    gb->tex_blur_a = make_target(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glGenFramebuffers(1, &gb->blur_fbo_a);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->blur_fbo_a);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->tex_blur_a, 0);
    { GLenum buf = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &buf); }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] bloom blur-a FBO incomplete: 0x%04x\n", status);

    gb->tex_blur_b = make_target(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glGenFramebuffers(1, &gb->blur_fbo_b);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->blur_fbo_b);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->tex_blur_b, 0);
    { GLenum buf = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &buf); }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] bloom blur-b FBO incomplete: 0x%04x\n", status);

    /* TAA: ping-pong pair of RGBA8 LDR targets (same format/size as
     * ldr_tex — TAA resolves the tonemapped, not HDR, result). */
    gb->taa_tex_a = make_target(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &gb->taa_fbo_a);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->taa_fbo_a);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->taa_tex_a, 0);
    { GLenum buf = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &buf); }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] TAA fbo A incomplete: 0x%04x\n", status);

    gb->taa_tex_b = make_target(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &gb->taa_fbo_b);
    glBindFramebuffer(GL_FRAMEBUFFER, gb->taa_fbo_b);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gb->taa_tex_b, 0);
    { GLenum buf = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &buf); }
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        printf("[gbuffer] TAA fbo B incomplete: 0x%04x\n", status);
    gb->taa_write_idx = 0;
    gb->taa_history_valid = 0;

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

    /* Small centered quad (roughly the middle 30% of the screen), separate
     * VBO from the fullscreen quad_vbo above — reuses quad_vao's binding
     * slot the same way gbuffer_render_shadow_map's mesh draw already
     * does (swap glBindBuffer + glVertexAttribPointer, restore after). */
    float tquad[] = { -0.15f,-0.15f,  0.15f,-0.15f,  0.15f,0.15f,
                       -0.15f,-0.15f,  0.15f,0.15f,  -0.15f,0.15f };
    glGenBuffers(1, &gb->transparent_test_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gb->transparent_test_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tquad), tquad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, gb->quad_vbo);
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
    gb->light_u_material      = glGetUniformLocation(gb->lighting_program, "u_material");
    gb->light_u_emissive      = glGetUniformLocation(gb->lighting_program, "u_emissive");
    gb->light_u_cam_pos       = glGetUniformLocation(gb->lighting_program, "u_cam_pos");

    gb->tonemap_program = link(QUAD_VERT_SRC, TONEMAP_FRAG_SRC);
    gb->tonemap_u_hdr = glGetUniformLocation(gb->tonemap_program, "u_hdr");

    gb->shadow_program = link(SHADOW_VERT_SRC, SHADOW_FRAG_SRC);
    gb->shadow_u_light_vp = glGetUniformLocation(gb->shadow_program, "u_light_vp");

    gb->fxaa_program = link(QUAD_VERT_SRC, FXAA_FRAG_SRC);
    gb->fxaa_u_tex        = glGetUniformLocation(gb->fxaa_program, "u_tex");
    gb->fxaa_u_resolution = glGetUniformLocation(gb->fxaa_program, "u_resolution");

    gb->brightpass_program = link(QUAD_VERT_SRC, BRIGHTPASS_FRAG_SRC);
    gb->bright_u_tex       = glGetUniformLocation(gb->brightpass_program, "u_tex");
    gb->bright_u_threshold = glGetUniformLocation(gb->brightpass_program, "u_threshold");

    gb->blur_program    = link(QUAD_VERT_SRC, BLUR_FRAG_SRC);
    gb->blur_u_tex        = glGetUniformLocation(gb->blur_program, "u_tex");
    gb->blur_u_texel_size = glGetUniformLocation(gb->blur_program, "u_texel_size");
    gb->blur_u_dir        = glGetUniformLocation(gb->blur_program, "u_dir");

    gb->composite_program = link(QUAD_VERT_SRC, COMPOSITE_FRAG_SRC);
    gb->composite_u_tex   = glGetUniformLocation(gb->composite_program, "u_tex");

    gb->transparent_test_program = link(TRANSPARENT_TEST_VERT_SRC, TRANSPARENT_TEST_FRAG_SRC);
    gb->transparent_test_u_color = glGetUniformLocation(gb->transparent_test_program, "u_color");

    gb->taa_program          = link(QUAD_VERT_SRC, TAA_FRAG_SRC);
    gb->taa_u_current        = glGetUniformLocation(gb->taa_program, "u_current");
    gb->taa_u_history        = glGetUniformLocation(gb->taa_program, "u_history");
    gb->taa_u_velocity       = glGetUniformLocation(gb->taa_program, "u_velocity");
    gb->taa_u_texel_size     = glGetUniformLocation(gb->taa_program, "u_texel_size");
    gb->taa_u_history_valid  = glGetUniformLocation(gb->taa_program, "u_history_valid");

    printf("[gbuffer] created %dx%d, lighting_prog=%u tonemap_prog=%u shadow_prog=%u "
           "fxaa_prog=%u bloom_progs=%u/%u/%u transparent_test_prog=%u taa_prog=%u (%dx%d)\n",
           w, h, gb->lighting_program, gb->tonemap_program, gb->shadow_program,
           gb->fxaa_program, gb->brightpass_program, gb->blur_program, gb->composite_program,
           gb->transparent_test_program, gb->taa_program, gb->shadow_size, gb->shadow_size);
    gl_check("gbuffer_create");
    return gb;
}

static void free_gl_resources(GBuffer *gb) {
    unsigned int texs[] = { gb->tex_albedo, gb->tex_normal, gb->tex_material, gb->tex_emissive,
                             gb->tex_velocity, gb->tex_object_id, gb->tex_depth_stencil, gb->hdr_tex,
                             gb->shadow_tex, gb->ldr_tex, gb->tex_bright, gb->tex_blur_a, gb->tex_blur_b,
                             gb->taa_tex_a, gb->taa_tex_b };
    glDeleteTextures((int)(sizeof(texs) / sizeof(texs[0])), texs);
    glDeleteFramebuffers(1, &gb->fbo);
    glDeleteFramebuffers(1, &gb->hdr_fbo);
    glDeleteFramebuffers(1, &gb->shadow_fbo);
    glDeleteFramebuffers(1, &gb->ldr_fbo);
    glDeleteFramebuffers(1, &gb->bright_fbo);
    glDeleteFramebuffers(1, &gb->blur_fbo_a);
    glDeleteFramebuffers(1, &gb->blur_fbo_b);
    glDeleteFramebuffers(1, &gb->taa_fbo_a);
    glDeleteFramebuffers(1, &gb->taa_fbo_b);
    glDeleteProgram(gb->lighting_program);
    glDeleteProgram(gb->tonemap_program);
    glDeleteProgram(gb->shadow_program);
    glDeleteProgram(gb->fxaa_program);
    glDeleteProgram(gb->brightpass_program);
    glDeleteProgram(gb->blur_program);
    glDeleteProgram(gb->composite_program);
    glDeleteProgram(gb->transparent_test_program);
    glDeleteProgram(gb->taa_program);
    /* transparent_test_vbo, like quad_vbo above, is intentionally not
     * deleted here — matches this function's existing pattern of never
     * freeing VBOs (glDeleteBuffers isn't in gl_native.h's proc list; the
     * GL context itself is torn down at process exit anyway). */
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

void gbuffer_set_viewport_offset(GBuffer *gb, int x, int y) {
    gb->vp_x = x;
    gb->vp_y = y;
}

void gbuffer_begin_geometry_pass(GBuffer *gb, const float *sky_color) {
    (void)sky_color;  /* background is handled in the lighting pass via depth, not by clearing color attachments here */
    glBindFramebuffer(GL_FRAMEBUFFER, gb->fbo);
    glViewport(0, 0, gb->w, gb->h);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    unsigned int no_object = 0xFFFFFFFFu;
    glClearBufferuiv(GL_COLOR, 5, &no_object);  /* draw-buffer index 5 = GL_COLOR_ATTACHMENT5 = object_id */
    /* gbuffer_resolve() (this same file, further down) ends its frame by
     * DISABLING GL_DEPTH_TEST for its own tonemap/FXAA fullscreen-quad
     * passes, which don't need it -- but never re-enables it before
     * returning, and GL state is persistent across frames/functions. The
     * geometry pass draws that happen here NEED depth testing, and per
     * the GL spec, when GL_DEPTH_TEST is disabled the depth buffer is
     * never updated AT ALL regardless of glDepthMask -- meaning every
     * frame's geometry silently stopped writing real depth values (while
     * still writing color/object-id normally, since those aren't gated
     * the same way), leaving depth permanently at its cleared far-plane
     * value. The lighting pass's "depth >= 0.999999 -> show sky, skip
     * lighting" background check then fired for genuinely-drawn geometry
     * too, compositing every object as flat sky color despite correct
     * object-id and albedo — this is what made a transform gizmo
     * (confirmed rasterizing correctly via a direct object-id readback)
     * and MeshObject rendering invisible, discovered while debugging gizmo
     * visibility. Explicitly (re-)enabling here, rather than trusting
     * whatever state a prior pass happened to leave, is the correct fix:
     * this pass owns its own required GL state instead of depending on
     * implicit persistence from unrelated code. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
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
                      const float *inv_view_proj, const float *cam_pos) {
    /* Real C-level render-pass hook, see render_hooks.h -- geometry+
     * shadow passes are already done (they run before gbuffer_resolve is
     * even called), nothing lit yet. */
    render_hooks_invoke(PHI_HOOK_AFTER_GBUFFER, gb);

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
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, gb->tex_material);
    glUniform1i(gb->light_u_material, 4);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, gb->tex_emissive);
    glUniform1i(gb->light_u_emissive, 5);
    glUniform3fv(gb->light_u_light_dir, 1, light_dir);
    glUniform3fv(gb->light_u_sky_color, 1, sky_color);
    glUniform3fv(gb->light_u_cam_pos, 1, cam_pos);
    glUniformMatrix4fv(gb->light_u_inv_view_proj, 1, GL_FALSE, inv_view_proj);
    glUniformMatrix4fv(gb->light_u_light_vp, 1, GL_FALSE, gb->light_vp);
    glBindVertexArray(gb->quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, gb->quad_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/lighting");

    /* ---- Bloom: threshold-extract, 2-pass separable blur, additive
     * composite back into hdr_tex — all before tonemap reads it. ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, gb->bright_fbo);
    glViewport(0, 0, gb->w, gb->h);
    glUseProgram(gb->brightpass_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->hdr_tex);
    glUniform1i(gb->bright_u_tex, 0);
    glUniform1f(gb->bright_u_threshold, 1.0f);
    glBindVertexArray(gb->quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/bloom_bright");

#ifndef __EMSCRIPTEN__
    /* One-shot sanity check, native only (same readPixels-portability
     * reasoning as the shadow-map diagnostic above): confirm the bright-
     * pass output is at/near zero everywhere, matching the honest
     * expectation documented in gbuffer.h — current lighting math never
     * exceeds ~1.0 HDR (no emissive materials, no over-bright lights),
     * so a >1.0 threshold should extract essentially nothing. This is
     * "correct plumbing with nothing to bloom yet", not a bug — this
     * check exists to prove that's actually true rather than assumed. */
    static int s_bright_checked = 0;
    if (!s_bright_checked) {
        s_bright_checked = 1;
        float px[4];
        glReadPixels(gb->w / 2, gb->h / 2, 1, 1, GL_RGBA, GL_FLOAT, px);
        printf("[gbuffer] bloom bright-pass center pixel = (%.4f,%.4f,%.4f) "
               "(expected ~0 under current content, threshold=1.0)\n", px[0], px[1], px[2]);
    }
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, gb->blur_fbo_a);
    glUseProgram(gb->blur_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->tex_bright);
    glUniform1i(gb->blur_u_tex, 0);
    glUniform2f(gb->blur_u_texel_size, 1.0f / (float)gb->w, 1.0f / (float)gb->h);
    glUniform2f(gb->blur_u_dir, 1.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/bloom_blur_h");

    glBindFramebuffer(GL_FRAMEBUFFER, gb->blur_fbo_b);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->tex_blur_a);
    glUniform1i(gb->blur_u_tex, 0);
    glUniform2f(gb->blur_u_dir, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/bloom_blur_v");

    glBindFramebuffer(GL_FRAMEBUFFER, gb->hdr_fbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(gb->composite_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->tex_blur_b);
    glUniform1i(gb->composite_u_tex, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/bloom_composite");
    glDisable(GL_BLEND);

    /* ---- Transparency: forward-blended pass into hdr_fbo, depth-tested
     * against the opaque scene but not depth-writing, so transparent draws
     * are correctly hidden behind opaque geometry without occluding each
     * other or the opaque pass. Still targeting hdr_fbo (already bound
     * from the bloom composite step above), so no extra
     * glBindFramebuffer/glViewport needed. See TRANSPARENT_TEST_VERT_SRC's
     * comment for why this is a fixed NDC-space probe quad rather than
     * real world content — there's nothing transparent in the game yet to
     * exercise this with.
     *
     * tex_depth_stencil is attached to hdr_fbo ONLY for this draw, not
     * permanently (see gbuffer_create's comment on hdr_tex) — the lighting
     * pass earlier in this function also binds hdr_fbo and SAMPLES this
     * same texture, which is an illegal feedback loop if it's attached at
     * that point (WebGL2/ANGLE correctly rejects it; desktop GL/Mesa
     * silently tolerated it, which is how this shipped once already and
     * broke lighting only in a real browser — see the user-reported "scene
     * is blank" bug this fixes). Detached again immediately after so
     * neither this frame's tonemap/fxaa nor next frame's lighting pass see
     * it attached. */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, gb->tex_depth_stencil, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* One-shot sanity check — draws a small semi-transparent NDC-space
     * probe quad near screen center, over whatever the scene already
     * rendered, and (native only) verifies the resulting blend against
     * the standard over-blend equation. This used to draw EVERY frame
     * forever: only the native readPixels+printf verification was gated
     * behind s_transparency_checked, not the glDrawArrays call itself —
     * so the probe quad permanently tinted/obscured the center of every
     * Scene panel render, on every platform, for this entire project
     * (that quiet reddish-magenta square visible in every screenshot this
     * whole session wasn't scene content, it was this). Found while
     * debugging why a transform gizmo positioned at the same on-screen
     * location wasn't visible despite the geometry pass genuinely
     * rasterizing it (confirmed via a direct object-id G-buffer
     * readback) — this quad was blending over it every single frame.
     * Now gated the same way the verification itself always was: draws
     * once, to prove the blend math actually works, then never again. */
    static int s_transparency_checked = 0;
    if (!s_transparency_checked) {
#ifndef __EMSCRIPTEN__
        /* Capture the HDR center pixel immediately BEFORE the blend
         * (background) and immediately AFTER (both within this same
         * gbuffer_resolve call, so scene content is identical between the
         * two reads — no frame-to-frame noise), confirming the "after"
         * value matches result = src*alpha + dst*(1-alpha) applied to the
         * captured background — proves the blend math is actually
         * happening at this exact pixel, not just assumed from the
         * state-setting calls below. */
        float t_bg[4] = {0,0,0,0};
        glReadPixels(gb->w / 2, gb->h / 2, 1, 1, GL_RGBA, GL_FLOAT, t_bg);
#endif

        glUseProgram(gb->transparent_test_program);
        glUniform4f(gb->transparent_test_u_color, 1.0f, 0.0f, 0.0f, 0.5f);
        glBindVertexArray(gb->quad_vao);
        glBindBuffer(GL_ARRAY_BUFFER, gb->transparent_test_vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        gl_check("gbuffer_resolve/transparency_test");
        glBindBuffer(GL_ARRAY_BUFFER, gb->quad_vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

        s_transparency_checked = 1;
#ifndef __EMSCRIPTEN__
        float t_after[4];
        glReadPixels(gb->w / 2, gb->h / 2, 1, 1, GL_RGBA, GL_FLOAT, t_after);
        float alpha = 0.5f;
        float exp_r = 1.0f * alpha + t_bg[0] * (1.0f - alpha);
        float exp_g = 0.0f * alpha + t_bg[1] * (1.0f - alpha);
        float exp_b = 0.0f * alpha + t_bg[2] * (1.0f - alpha);
        printf("[gbuffer] transparency blend check: bg=(%.4f,%.4f,%.4f) "
               "expected=(%.4f,%.4f,%.4f) actual=(%.4f,%.4f,%.4f)\n",
               t_bg[0], t_bg[1], t_bg[2], exp_r, exp_g, exp_b,
               t_after[0], t_after[1], t_after[2]);
#endif
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

    /* Real C-level render-pass hook, see render_hooks.h -- Lighting,
     * Bloom, and Transparency have all completed into gb->hdr_tex by
     * this point; nothing tonemapped yet. */
    render_hooks_invoke(PHI_HOOK_AFTER_LIGHTING, gb);

    /* ---- Tonemap: HDR -> intermediate LDR texture (not the default
     * framebuffer directly — fxaa below needs to read the tonemapped
     * result before it's actually presented). ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, gb->ldr_fbo);
    glViewport(0, 0, gb->w, gb->h);
    glUseProgram(gb->tonemap_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->hdr_tex);
    glUniform1i(gb->tonemap_u_hdr, 0);
    glBindVertexArray(gb->quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/tonemap");

    /* Real C-level render-pass hook, see render_hooks.h -- gb->ldr_tex
     * holds the tonemapped-but-not-yet-temporally-resolved frame here. */
    render_hooks_invoke(PHI_HOOK_AFTER_RESOLVE, gb);

    /* ---- TAA: temporal resolve (tonemapped LDR + velocity-reprojected,
     * neighborhood-clamped history) -> one of the ping-pong targets. FXAA
     * below reads from that instead of ldr_tex directly. See
     * gbuffer.h's taa_tex_a/b comment for the camera-motion-only velocity
     * caveat and TAA_FRAG_SRC for the blend/clamp formulation. ---- */
    unsigned int taa_write_fbo = gb->taa_write_idx == 0 ? gb->taa_fbo_a : gb->taa_fbo_b;
    unsigned int taa_write_tex = gb->taa_write_idx == 0 ? gb->taa_tex_a : gb->taa_tex_b;
    unsigned int taa_read_tex  = gb->taa_write_idx == 0 ? gb->taa_tex_b : gb->taa_tex_a;

#ifndef __EMSCRIPTEN__
    /* Only referenced by the native-only diagnostic below (to reread the
     * history texture's own FBO for a readPixels comparison) — declared
     * here rather than unconditionally above to avoid an unused-variable
     * warning on wasm. */
    unsigned int taa_read_fbo = gb->taa_write_idx == 0 ? gb->taa_fbo_b : gb->taa_fbo_a;
    /* One-shot sanity check, native only (same readPixels-portability
     * reasoning as the other diagnostics in this file), on the first frame
     * where history is actually valid: reconstruct the exact same
     * neighborhood-clamp + blend math on the CPU from raw texel reads
     * (ldr_tex's center + 4-neighbor taps, the reprojected history texel
     * via NEAREST sampling — all taa textures are NEAREST/CLAMP_TO_EDGE,
     * see make_target — and tex_velocity), then compare against the GPU's
     * actual output at that pixel. Skipped (retried next frame) if the
     * reprojected sample this particular frame happens to fall off-screen,
     * since that takes the shader's early-return path instead. */
    static int s_taa_checked = 0;
    static int s_taa_calls = 0;
    s_taa_calls++;
    int taa_do_check = 0;
    unsigned char taa_cur_px[4], taa_nl_px[4], taa_nr_px[4], taa_nu_px[4], taa_nd_px[4], taa_hist_px[4];
    float taa_vel_px[4] = {0,0,0,0};
    /* Wait a few calls past startup rather than checking on the very
     * first history-valid frame — mostly cosmetic (either a zero- or
     * non-zero-velocity result is a valid, honestly-reported check), but
     * gives real camera movement a better chance of already being
     * underway. Verified against both cases during development: a
     * synthetic-input test (see the session's git log) caught this
     * matching bit-exact with real non-zero velocity
     * (vel=(0.00001,0.01775)), not just the trivial zero-velocity case. */
    if (!s_taa_checked && gb->taa_history_valid && s_taa_calls > 5) {
        int cx = gb->w / 2, cy = gb->h / 2;
        glBindFramebuffer(GL_FRAMEBUFFER, gb->ldr_fbo);
        glReadPixels(cx,     cy,     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, taa_cur_px);
        glReadPixels(cx - 1, cy,     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, taa_nl_px);
        glReadPixels(cx + 1, cy,     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, taa_nr_px);
        glReadPixels(cx,     cy + 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, taa_nu_px);
        glReadPixels(cx,     cy - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, taa_nd_px);

        glBindFramebuffer(GL_FRAMEBUFFER, gb->fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT4);
        glReadPixels(cx, cy, 1, 1, GL_RGBA, GL_FLOAT, taa_vel_px);

        float u_cur = (cx + 0.5f) / (float)gb->w;
        float v_cur = (cy + 0.5f) / (float)gb->h;
        float prev_u = u_cur - taa_vel_px[0];
        float prev_v = v_cur - taa_vel_px[1];
        int off_screen = prev_u < 0.0f || prev_u > 1.0f || prev_v < 0.0f || prev_v > 1.0f;
        if (!off_screen) {
            int px = (int)(prev_u * gb->w); if (px >= gb->w) px = gb->w - 1; if (px < 0) px = 0;
            int py = (int)(prev_v * gb->h); if (py >= gb->h) py = gb->h - 1; if (py < 0) py = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, taa_read_fbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, taa_hist_px);
            taa_do_check = 1;
        }
    }
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, taa_write_fbo);
    glViewport(0, 0, gb->w, gb->h);
    glUseProgram(gb->taa_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gb->ldr_tex);
    glUniform1i(gb->taa_u_current, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, taa_read_tex);
    glUniform1i(gb->taa_u_history, 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gb->tex_velocity);
    glUniform1i(gb->taa_u_velocity, 2);
    glUniform2f(gb->taa_u_texel_size, 1.0f / (float)gb->w, 1.0f / (float)gb->h);
    glUniform1f(gb->taa_u_history_valid, gb->taa_history_valid ? 1.0f : 0.0f);
    glBindVertexArray(gb->quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/taa");

#ifndef __EMSCRIPTEN__
    if (taa_do_check) {
        s_taa_checked = 1;
        float cur[3]  = { taa_cur_px[0]/255.0f, taa_cur_px[1]/255.0f, taa_cur_px[2]/255.0f };
        float hist[3] = { taa_hist_px[0]/255.0f, taa_hist_px[1]/255.0f, taa_hist_px[2]/255.0f };
        float nl[3] = { taa_nl_px[0]/255.0f, taa_nl_px[1]/255.0f, taa_nl_px[2]/255.0f };
        float nr[3] = { taa_nr_px[0]/255.0f, taa_nr_px[1]/255.0f, taa_nr_px[2]/255.0f };
        float nu[3] = { taa_nu_px[0]/255.0f, taa_nu_px[1]/255.0f, taa_nu_px[2]/255.0f };
        float nd[3] = { taa_nd_px[0]/255.0f, taa_nd_px[1]/255.0f, taa_nd_px[2]/255.0f };
        float expected[3];
        for (int i = 0; i < 3; i++) {
            float nmin = cur[i], nmax = cur[i];
            if (nl[i] < nmin) nmin = nl[i];
            if (nr[i] < nmin) nmin = nr[i];
            if (nu[i] < nmin) nmin = nu[i];
            if (nd[i] < nmin) nmin = nd[i];
            if (nl[i] > nmax) nmax = nl[i];
            if (nr[i] > nmax) nmax = nr[i];
            if (nu[i] > nmax) nmax = nu[i];
            if (nd[i] > nmax) nmax = nd[i];
            float clamped = hist[i];
            if (clamped < nmin) clamped = nmin;
            if (clamped > nmax) clamped = nmax;
            expected[i] = cur[i] * 0.1f + clamped * 0.9f;
        }
        unsigned char actual_px[4];
        glBindFramebuffer(GL_FRAMEBUFFER, taa_write_fbo);
        glReadPixels(gb->w / 2, gb->h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual_px);
        float actual[3] = { actual_px[0]/255.0f, actual_px[1]/255.0f, actual_px[2]/255.0f };
        printf("[gbuffer] TAA blend check: vel=(%.5f,%.5f) expected=(%.4f,%.4f,%.4f) "
               "actual=(%.4f,%.4f,%.4f) (8-bit quantization means ~1/255 slack is expected)\n",
               taa_vel_px[0], taa_vel_px[1], expected[0], expected[1], expected[2],
               actual[0], actual[1], actual[2]);
    }
#endif

    gb->taa_write_idx ^= 1;
    gb->taa_history_valid = 1;

    /* ---- FXAA: TAA-resolved texture -> default framebuffer ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* vp_x/vp_y (default 0,0): where in the actual window this lands —
     * every earlier pass in this function renders into its OWN private
     * texture at (0,0) within that texture, so only this final blit into
     * the shared default framebuffer needs an offset, for hosting the 3D
     * scene inside an arbitrary sub-rectangle (the UI system's Scene
     * panel) instead of always filling the whole window. See
     * gbuffer_set_viewport_offset(). */
    glViewport(gb->vp_x, gb->vp_y, gb->w, gb->h);
    glUseProgram(gb->fxaa_program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, taa_write_tex);
    glUniform1i(gb->fxaa_u_tex, 0);
    glUniform2f(gb->fxaa_u_resolution, (float)gb->w, (float)gb->h);
    glBindVertexArray(gb->quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_check("gbuffer_resolve/fxaa");

    glEnable(GL_DEPTH_TEST);

    /* Real C-level render-pass hook, see render_hooks.h -- the very last
     * point before this frame is presented (matches phi.md's original
     * "after [fxaa], final output" placement for this specific name). */
    render_hooks_invoke(PHI_HOOK_AFTER_TONEMAP, gb);
}

unsigned int gbuffer_pick_object_id(GBuffer *gb, int x, int y) {
    glBindFramebuffer(GL_FRAMEBUFFER, gb->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT5);
    unsigned int id = 0xFFFFFFFFu;
    glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return id;
}
