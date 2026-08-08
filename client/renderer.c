#include "renderer.h"
#include "octree_render.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>   /* not GLES2/gl2.h — need glUniform1ui, MRT outputs (GLES2/WebGL1 had neither) */
#include <emscripten.h>
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif

/* ---- Shaders ----
 * Two variants: GLSL ES 3.00 (WebGL2/GLES3 — in/out, explicit frag_color,
 * still needs a precision qualifier unlike desktop core profile) and GLSL
 * 330 core (in/out, explicit frag_color, no precision qualifiers — core
 * profile dropped those). Same lighting logic, same attribute/uniform
 * names (bound to matching locations either way via glBindAttribLocation
 * before linking, see link_program), so everything downstream of
 * link_program is identical on both backends. GLSL ES 1.00
 * (attribute/varying/gl_FragColor) was retired along with the WebGL1
 * context — see phi.md's "WebGL 2 from day one" decision. */
#ifdef __EMSCRIPTEN__
static const char *VERT_SRC =
    "#version 300 es\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "in float a_mat_id;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prev_mvp;\n"
    "out vec3 v_normal;\n"
    "out float v_mat_id;\n"
    /* Un-divided clip xy + w for both this frame and the reprojected
     * previous frame (see prev_mvp's comment in renderer.h) — .z carries
     * clip.w, divided in the fragment shader rather than here so the
     * rasterizer's perspective-correct interpolation reconstructs v_clip_curr
     * exactly (its own w and gl_Position.w are the same value). v_clip_prev
     * uses a different underlying w, so interpolating it against
     * gl_Position.w's weighting is an approximation, not exact — acceptable
     * for a first-pass velocity buffer, see gbuffer.h's TAA comment. */
    "out vec3 v_clip_curr;\n"
    "out vec3 v_clip_prev;\n"
    "void main() {\n"
    "  vec4 clip = u_mvp * vec4(a_pos, 1.0);\n"
    "  gl_Position = clip;\n"
    "  v_clip_curr = vec3(clip.xy, clip.w);\n"
    "  vec4 clip_prev = u_prev_mvp * vec4(a_pos, 1.0);\n"
    "  v_clip_prev = vec3(clip_prev.xy, clip_prev.w);\n"
    "  v_normal  = a_normal;\n"
    "  v_mat_id  = a_mat_id;\n"
    "}\n";

/* Geometry-pass output — writes the G-buffer (see gbuffer.h) instead of a
 * lit color directly, same as the native variant below (see its comment
 * for the material/emissive/velocity channel status). */
static const char *FRAG_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3  v_normal;\n"
    "in float v_mat_id;\n"
    "in vec3  v_clip_curr;\n"
    "in vec3  v_clip_prev;\n"
    "uniform vec3  u_mat_color;\n"
    "uniform uint  u_object_id;\n"
    "layout(location=0) out vec4 out_albedo;\n"
    "layout(location=1) out vec4 out_normal;\n"
    "layout(location=2) out vec4 out_material;\n"
    "layout(location=3) out vec4 out_emissive;\n"
    "layout(location=4) out vec4 out_velocity;\n"
    "layout(location=5) out uint out_object_id;\n"
    "void main() {\n"
    "  vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);\n"
    "  out_albedo    = vec4(u_mat_color, 1.0);\n"
    "  out_normal    = vec4(n * 0.5 + 0.5, 0.0);\n"
    "  out_material  = vec4(0.5, 0.0, 0.0, 0.0);\n"
    "  out_emissive  = vec4(0.0);\n"
    /* Screen-space UV-space motion vector: current NDC minus reprojected-
     * previous-frame NDC (see prev_mvp's comment in renderer.h — camera
     * motion only, not per-object motion), scaled by 0.5 since NDC's
     * [-1,1] range maps to UV's [0,1] range at half the extent. */
    "  vec2 ndc_curr = v_clip_curr.xy / v_clip_curr.z;\n"
    "  vec2 ndc_prev = v_clip_prev.xy / v_clip_prev.z;\n"
    "  out_velocity  = vec4((ndc_curr - ndc_prev) * 0.5, 0.0, 0.0);\n"
    "  out_object_id = u_object_id;\n"
    "}\n";
#else
static const char *VERT_SRC =
    "#version 330 core\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "in float a_mat_id;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prev_mvp;\n"
    "out vec3 v_normal;\n"
    "out float v_mat_id;\n"
    /* Un-divided clip xy + w for both this frame and the reprojected
     * previous frame (see prev_mvp's comment in renderer.h) — .z carries
     * clip.w, divided in the fragment shader rather than here so the
     * rasterizer's perspective-correct interpolation reconstructs v_clip_curr
     * exactly (its own w and gl_Position.w are the same value). v_clip_prev
     * uses a different underlying w, so interpolating it against
     * gl_Position.w's weighting is an approximation, not exact — acceptable
     * for a first-pass velocity buffer, see gbuffer.h's TAA comment. */
    "out vec3 v_clip_curr;\n"
    "out vec3 v_clip_prev;\n"
    "void main() {\n"
    "  vec4 clip = u_mvp * vec4(a_pos, 1.0);\n"
    "  gl_Position = clip;\n"
    "  v_clip_curr = vec3(clip.xy, clip.w);\n"
    "  vec4 clip_prev = u_prev_mvp * vec4(a_pos, 1.0);\n"
    "  v_clip_prev = vec3(clip_prev.xy, clip_prev.w);\n"
    "  v_normal  = a_normal;\n"
    "  v_mat_id  = a_mat_id;\n"
    "}\n";

/* Geometry-pass output — writes the G-buffer (see gbuffer.h) instead of a
 * lit color directly. Lighting moves to gbuffer.c's separate lighting pass,
 * which reads out_albedo/out_normal back as textures. material/emissive
 * still get honest placeholder defaults (no PBR params or emissive
 * surfaces exist yet); velocity is now real (camera-motion-only, see
 * v_clip_curr/v_clip_prev above and gbuffer.h's TAA comment for the
 * per-object-motion caveat), used by gbuffer.c's TAA resolve pass. */
static const char *FRAG_SRC =
    "#version 330 core\n"
    "in vec3  v_normal;\n"
    "in float v_mat_id;\n"
    "in vec3  v_clip_curr;\n"
    "in vec3  v_clip_prev;\n"
    "uniform vec3  u_mat_color;\n"
    "uniform uint  u_object_id;\n"
    "layout(location=0) out vec4 out_albedo;\n"
    "layout(location=1) out vec4 out_normal;\n"
    "layout(location=2) out vec4 out_material;\n"
    "layout(location=3) out vec4 out_emissive;\n"
    "layout(location=4) out vec4 out_velocity;\n"
    "layout(location=5) out uint out_object_id;\n"
    "void main() {\n"
    "  vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);\n"
    "  out_albedo    = vec4(u_mat_color, 1.0);\n"
    "  out_normal    = vec4(n * 0.5 + 0.5, 0.0);\n"
    "  out_material  = vec4(0.5, 0.0, 0.0, 0.0);\n"
    "  out_emissive  = vec4(0.0);\n"
    /* Screen-space UV-space motion vector: current NDC minus reprojected-
     * previous-frame NDC (see prev_mvp's comment in renderer.h — camera
     * motion only, not per-object motion), scaled by 0.5 since NDC's
     * [-1,1] range maps to UV's [0,1] range at half the extent. */
    "  vec2 ndc_curr = v_clip_curr.xy / v_clip_curr.z;\n"
    "  vec2 ndc_prev = v_clip_prev.xy / v_clip_prev.z;\n"
    "  out_velocity  = vec4((ndc_curr - ndc_prev) * 0.5, 0.0, 0.0);\n"
    "  out_object_id = u_object_id;\n"
    "}\n";
#endif

/* Mirrors whatever was last passed to glClearColor by renderer_create /
 * renderer_set_sky_color — glClearColor itself is opaque GL state with no
 * getter, but gbuffer.c's lighting pass needs the actual current sky color
 * (not a hardcoded guess) to paint background pixels, since the deferred
 * path no longer relies on glClearColor's implicit background-fill. */
static float s_sky_color[3] = {0.3f, 0.5f, 0.8f};

/* ===========================================================
 * Column-major mat4  (OpenGL convention)
 * Storage: m[col*4 + row]
 *
 *   m[0]  m[4]  m[8]  m[12]
 *   m[1]  m[5]  m[9]  m[13]
 *   m[2]  m[6]  m[10] m[14]
 *   m[3]  m[7]  m[11] m[15]
 * =========================================================== */

void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_mul(float *out, const float *a, const float *b) {
    float tmp[16];
    for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++) {
        float s = 0;
        for (int k = 0; k < 4; k++)
            s += a[k*4 + row] * b[col*4 + k];
        tmp[col*4 + row] = s;
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_perspective(float *m, float fovy, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

static void mat4_look_dir(float *m,
                           float px, float py, float pz,
                           float yaw, float pitch) {
    float sy = sinf(yaw),  cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);

    float rx =  cy,       ry = 0.0f, rz = -sy;
    float ux = sy*sp,     uy = cp,   uz =  cy*sp;
    float fx = -sy*cp,    fy = sp,   fz = -cy*cp;

    m[0]  = rx;   m[4]  = ry;   m[8]  = rz;   m[12] = -(rx*px + ry*py + rz*pz);
    m[1]  = ux;   m[5]  = uy;   m[9]  = uz;   m[13] = -(ux*px + uy*py + uz*pz);
    m[2]  = -fx;  m[6]  = -fy;  m[10] = -fz;  m[14] =  (fx*px + fy*py + fz*pz);
    m[3]  = 0.0f; m[7]  = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

static void mat4_translate(float *m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx; m[13] = ty; m[14] = tz;
}

/* ---- GL error helper ---- */
static void gl_check(const char *where) {
    /* Drains every pending error, not just one — glGetError only returns
     * (and clears) a single error per call, so if multiple had queued up
     * (e.g. from a diagnostic readback earlier in the frame), a single
     * check here would report just one and leave the rest to be
     * misattributed to whatever the NEXT gl_check() call happens to be,
     * anywhere in the codebase. Found exactly this happening: a bad
     * readPixels format in gbuffer.c's shadow-map diagnostic left a
     * stale error that then got blamed on gbuffer_resolve's lighting
     * pass, several calls later. */
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR)
        printf("[GL] error 0x%04x at %s\n", e, where);
}

/* Re-binds this renderer's VAO before every draw rather than trusting it to
 * stay bound (GLES2/WebGL1 has no VAO concept, so this used to be a no-op
 * on wasm — now wasm is WebGL2/GLES3, which does have one, and gbuffer.c's
 * fullscreen-quad passes bind their own VAO on both platforms). It's NOT
 * safe to assume "bind once at renderer_create and never again" on either
 * target: gbuffer.c's lighting/tonemap draws bind their own VAO every
 * frame, which otherwise silently steals the binding out from under the
 * next frame's geometry-pass draw calls — this exact bug already happened
 * once on native before every draw call here was made to defend against
 * it; wasm needed the same fix once it started sharing gbuffer.c too. */
static void bind_renderer_vao(const Renderer *r) {
    glBindVertexArray(r->vao);
}

/* ---- Shader compilation ---- */
static unsigned int compile_shader(GLenum type, const char *src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
        printf("[renderer] Shader compile error: %s\n", log);
    }
    return s;
}

static unsigned int link_program(const char *vsrc, const char *fsrc) {
    unsigned int vs = compile_shader(GL_VERTEX_SHADER,   vsrc);
    unsigned int fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    unsigned int p  = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    /* Bind locations before linking */
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_normal");
    glBindAttribLocation(p, 2, "a_mat_id");
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, NULL, log);
        printf("[renderer] Program link error: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    gl_check("link_program");
    return p;
}

/* ---- Box VBO (pos+normal, 6 floats/vert, 36 verts) ---- */
static void build_box_vbo(unsigned int *vbo, float hw, float h, float hd) {
    float verts[36 * 6];
    float *p = verts;
#define PUSH(px,py,pz,nx,ny,nz) \
    do{*p++=(px);*p++=(py);*p++=(pz);*p++=(nx);*p++=(ny);*p++=(nz);}while(0)
#define QUAD(ax,ay,az,bx,by,bz,cx,cy,cz,dx,dy,dz,nx,ny,nz) do{ \
    PUSH(ax,ay,az,nx,ny,nz);PUSH(bx,by,bz,nx,ny,nz);PUSH(cx,cy,cz,nx,ny,nz); \
    PUSH(ax,ay,az,nx,ny,nz);PUSH(cx,cy,cz,nx,ny,nz);PUSH(dx,dy,dz,nx,ny,nz);}while(0)
    QUAD(-hw,0,-hd, -hw,h,-hd, -hw,h,hd,  -hw,0,hd,   -1,0,0);
    QUAD( hw,0, hd,  hw,h, hd,  hw,h,-hd,  hw,0,-hd,   1,0,0);
    QUAD(-hw,0, hd,  hw,0, hd,  hw,0,-hd, -hw,0,-hd,   0,-1,0);
    QUAD(-hw,h,-hd,  hw,h,-hd,  hw,h, hd, -hw,h, hd,   0,1,0);
    QUAD(-hw,0,-hd,  hw,0,-hd,  hw,h,-hd, -hw,h,-hd,   0,0,-1);
    QUAD( hw,0, hd, -hw,0, hd, -hw,h, hd,  hw,h, hd,   0,0,1);
#undef QUAD
#undef PUSH
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    gl_check("build_box_vbo");
}

/* Same as build_box_vbo(), but Y spans [-h/2, h/2] instead of [0, h] — i.e.
 * the local origin is the box's true center, not its base. build_box_vbo's
 * "sits on top of position" convention is correct for players (position is
 * feet) but wrong for anything that ROTATES (like the rocket): rotating a
 * base-anchored box swings that offset to point in whatever direction the
 * rotation's local +Y maps to, instead of staying "above" in world space —
 * i.e. the rendered box visibly drifts away from the object's true (correct)
 * physics position depending on which way it's travelling.
 *
 * (No GPU readback here — glGetBufferSubData doesn't exist in GLES2/WebGL1,
 * which is this project's target; the geometry is just regenerated with a
 * shifted Y range instead of reusing build_box_vbo's upload.) */
static void build_box_vbo_centered(unsigned int *vbo, float hw, float h, float hd) {
    float h0 = -h * 0.5f, h1 = h * 0.5f;
    float verts[36 * 6];
    float *p = verts;
#define PUSH(px,py,pz,nx,ny,nz) \
    do{*p++=(px);*p++=(py);*p++=(pz);*p++=(nx);*p++=(ny);*p++=(nz);}while(0)
#define QUAD(ax,ay,az,bx,by,bz,cx,cy,cz,dx,dy,dz,nx,ny,nz) do{ \
    PUSH(ax,ay,az,nx,ny,nz);PUSH(bx,by,bz,nx,ny,nz);PUSH(cx,cy,cz,nx,ny,nz); \
    PUSH(ax,ay,az,nx,ny,nz);PUSH(cx,cy,cz,nx,ny,nz);PUSH(dx,dy,dz,nx,ny,nz);}while(0)
    QUAD(-hw,h0,-hd, -hw,h1,-hd, -hw,h1,hd,  -hw,h0,hd,   -1,0,0);
    QUAD( hw,h0, hd,  hw,h1, hd,  hw,h1,-hd,  hw,h0,-hd,   1,0,0);
    QUAD(-hw,h0, hd,  hw,h0, hd,  hw,h0,-hd, -hw,h0,-hd,   0,-1,0);
    QUAD(-hw,h1,-hd,  hw,h1,-hd,  hw,h1, hd, -hw,h1, hd,   0,1,0);
    QUAD(-hw,h0,-hd,  hw,h0,-hd,  hw,h1,-hd, -hw,h1,-hd,   0,0,-1);
    QUAD( hw,h0, hd, -hw,h0, hd, -hw,h1, hd,  hw,h1, hd,   0,0,1);
#undef QUAD
#undef PUSH
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    gl_check("build_box_vbo_centered");
}

/* ---- Material palette ---- */
static void init_palette(Renderer *r) {
    static const float pal[][3] = {
        {0.50f,0.50f,0.55f},{0.55f,0.40f,0.25f},{0.30f,0.55f,0.25f},
        {0.65f,0.60f,0.50f},{0.25f,0.30f,0.65f},{0.70f,0.20f,0.20f},
        {0.80f,0.75f,0.60f},{0.20f,0.20f,0.20f},
    };
    int n = (int)(sizeof(pal)/sizeof(pal[0]));
    for (int i = 0; i < 256; i++) {
        r->palette[i][0] = pal[i%n][0];
        r->palette[i][1] = pal[i%n][1];
        r->palette[i][2] = pal[i%n][2];
    }
}

static void build_vp(const Renderer *r, float *vp);  /* defined below, needed by renderer_create/renderer_end_frame */

/* ================================================================
 * Public API
 * ================================================================ */

Renderer *renderer_create(int width, int height) {
    Renderer *r = (Renderer *)calloc(1, sizeof(Renderer));
    r->fov_y = 75.0f * (float)M_PI / 180.0f;

#ifndef __EMSCRIPTEN__
    /* Must run before any GL call below that isn't in <GL/gl.h>'s GL 1.2
     * set — wasm doesn't need this at all, GLES3 functions link directly
     * against Emscripten's GL library, no proc-address fetching required. */
    gl_native_load_procs();
#endif
    /* GL 3.3 core requires a bound (non-zero) VAO for any vertex-attrib /
     * draw call; GLES3/WebGL2 doesn't strictly require one (VAO 0 is
     * legal there, unlike desktop core profile) but creating an explicit
     * one on both platforms and re-binding it before every draw
     * (bind_renderer_vao) is what actually keeps gbuffer.c's own VAO
     * usage from silently stealing the binding — see that function's
     * comment. One VAO for the whole renderer's lifetime, not one per
     * mesh, since every draw call here already re-specifies its own
     * attrib pointers each time. */
    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    /* No back-face culling: noclip editor flight lets the camera end up
     * behind/inside geometry and bot boxes (impossible in normal collided
     * play), so both winding directions of every triangle must rasterize
     * or those faces just vanish from certain angles. */
    glClearColor(s_sky_color[0], s_sky_color[1], s_sky_color[2], 1.0f);

    r->program     = link_program(VERT_SRC, FRAG_SRC);
    r->u_mvp       = glGetUniformLocation(r->program, "u_mvp");
    r->u_prev_mvp  = glGetUniformLocation(r->program, "u_prev_mvp");
    r->u_light_dir = glGetUniformLocation(r->program, "u_light_dir");
    r->u_mat_color = glGetUniformLocation(r->program, "u_mat_color");
    r->u_object_id = glGetUniformLocation(r->program, "u_object_id");
    r->cur_object_id = 0;
    r->a_pos    = 0;
    r->a_normal = 1;
    r->a_mat_id = 2;

    printf("[renderer] prog=%u mvp=%d prev_mvp=%d ldir=%d mcol=%d\n",
           r->program, r->u_mvp, r->u_prev_mvp, r->u_light_dir, r->u_mat_color);

    build_box_vbo_centered(&r->rocket_vbo, 3.0f, 3.0f, 12.0f);
    init_palette(r);
    renderer_resize(r, width, height);
    /* Seed prev_vp with this frame's own vp — gives exactly zero velocity
     * on the very first frame (nothing to compare against yet) rather than
     * a garbage/zero-matrix reprojection. */
    build_vp(r, r->prev_vp);
    return r;
}

void renderer_end_frame(Renderer *r) {
    build_vp(r, r->prev_vp);
}

void renderer_destroy(Renderer *r) {
    glDeleteProgram(r->program);
    free(r);
}

void renderer_set_fov(Renderer *r, float degrees) {
    if (degrees < 30.0f)  degrees = 30.0f;
    if (degrees > 150.0f) degrees = 150.0f;
    r->fov_y = degrees * (float)M_PI / 180.0f;
}

void renderer_set_sky_color(float r, float g, float b) {
    s_sky_color[0] = r; s_sky_color[1] = g; s_sky_color[2] = b;
    glClearColor(r, g, b, 1.0f);
}

void renderer_get_sky_color(float *out3) {
    out3[0] = s_sky_color[0]; out3[1] = s_sky_color[1]; out3[2] = s_sky_color[2];
}

void renderer_resize(Renderer *r, int w, int h) {
    r->vp_w = w ? w : 1;
    r->vp_h = h ? h : 1;
    glViewport(0, 0, r->vp_w, r->vp_h);
}

void renderer_set_camera(Renderer *r, const Player *p) {
    r->cam_pos[0] = p->pos.x;
    r->cam_pos[1] = p->pos.y + player_eye_h(p);
    r->cam_pos[2] = p->pos.z;
    r->cam_yaw    = p->yaw;
    r->cam_pitch  = p->pitch;
}

void renderer_set_object_id(Renderer *r, unsigned int id) {
    r->cur_object_id = id;
}

static void build_vp(const Renderer *r, float *vp) {
    float proj[16], view[16];
    mat4_perspective(proj, r->fov_y,
                     (float)r->vp_w / (float)r->vp_h,
                     1.0f, 4096.0f);
    mat4_look_dir(view,
                  r->cam_pos[0], r->cam_pos[1], r->cam_pos[2],
                  r->cam_yaw, r->cam_pitch);
    mat4_mul(vp, proj, view);
}

/* General 4x4 inverse via cofactor expansion — column-major, same layout
 * every matrix in this file uses. Verified numerically before use here
 * (VP * VPinv == identity, and a world point round-trips exactly through
 * clip space and back) rather than trusted on inspection alone, given
 * this file's history of subtle basis/handedness bugs in exactly this
 * kind of matrix code (see mat4_look_rotation's comments). */
static int mat4_inverse(const float *m, float *inv) {
    float det;
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-8f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) inv[i] *= det;
    return 1;
}

int renderer_get_inverse_view_proj(const Renderer *r, float *out16) {
    float vp[16];
    build_vp(r, vp);
    return mat4_inverse(vp, out16);
}

void renderer_draw_world(Renderer *r, RenderMesh *mesh) {
    if (!mesh || mesh->count == 0) { printf("[renderer] mesh empty\n"); return; }

    mesh_upload(mesh);
    if (!mesh->vbo) { printf("[renderer] vbo=0 after upload\n"); return; }

    float vp[16];
    build_vp(r, vp);

    r->cur_object_id = 0;

    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, vp);
    /* World mesh vertices are already in world space (no model matrix), so
     * the "previous frame" reprojection is just prev_vp directly — see
     * prev_vp's comment in renderer.h. */
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, r->prev_vp);
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    glUniform3f(r->u_mat_color, 0.50f, 0.50f, 0.55f);
    /* Safe on both backends now that wasm is WebGL2/GLES3, which has
     * glUniform1ui (GLES2/WebGL1 didn't — GLSL ES 1.00 has no uint type,
     * and this used to need a #ifndef __EMSCRIPTEN__ guard for exactly
     * that reason). */
    glUniform1ui(r->u_object_id, r->cur_object_id);

    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    int stride = VERTEX_STRIDE * (int)sizeof(float);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, mesh->count);
    gl_check("draw_world");

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
}

static void draw_box(const Renderer *r, unsigned int vbo,
                     float px, float py, float pz,
                     float cr, float cg, float cb,
                     const float *vp) {
    float t[16], mvp[16], prev_mvp[16];
    mat4_translate(t, px, py, pz);
    mat4_mul(mvp, vp, t);
    /* Camera-motion-only scope (see prev_vp's comment in renderer.h):
     * reuses this frame's own (current) model matrix t for the "previous"
     * reprojection too, so this object's own movement isn't captured, only
     * parallax from camera motion. */
    mat4_mul(prev_mvp, r->prev_vp, t);

    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, prev_mvp);
    glUniform3f(r->u_mat_color, cr, cg, cb);
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    /* Safe on both backends now that wasm is WebGL2/GLES3, which has
     * glUniform1ui (GLES2/WebGL1 didn't — GLSL ES 1.00 has no uint type,
     * and this used to need a #ifndef __EMSCRIPTEN__ guard for exactly
     * that reason). */
    glUniform1ui(r->u_object_id, r->cur_object_id);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    int stride = 6 * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 0.0f);

    glDrawArrays(GL_TRIANGLES, 0, 36);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

/* Rotation that maps local +Z (the rocket box's long axis, see
 * build_box_vbo(3,3,12) in renderer_create) onto a direction vector, so a
 * drawn rocket visibly points the way it's actually travelling instead of
 * always rendering axis-aligned regardless of aim. */
static void mat4_look_rotation(float *m, float dx, float dy, float dz) {
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 1e-6f) { mat4_identity(m); return; }
    float fx = dx/len, fy = dy/len, fz = dz/len;

    float upx = 0.0f, upy = 1.0f, upz = 0.0f;
    if (fabsf(fx*upx + fy*upy + fz*upz) > 0.999f) { upx = 1.0f; upy = 0.0f; upz = 0.0f; }

    /* cross(forward, up), not cross(up, forward) — the latter (what this
     * used to compute) comes out as the exact negative of mat4_look_dir's
     * right vector at every yaw/pitch, i.e. this basis was mirrored
     * left-right relative to the camera's own convention. */
    float rx = fy*upz - fz*upy, ry = fz*upx - fx*upz, rz = fx*upy - fy*upx;
    float rl = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= rl; ry /= rl; rz /= rl;

    /* cross(right, forward), not cross(forward, right) — swapping the right
     * vector's cross-product order above flips its sign, which cascades
     * into this one too if left as-is; swap this order as well to cancel
     * it back out so both axes match the camera basis exactly. */
    float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;

    mat4_identity(m);
    m[0] = rx; m[1] = ry; m[2] = rz;
    m[4] = ux; m[5] = uy; m[6] = uz;
    m[8] = fx; m[9] = fy; m[10] = fz;
}

static void draw_box_oriented(const Renderer *r, unsigned int vbo,
                              float px, float py, float pz,
                              float dx, float dy, float dz,
                              float cr, float cg, float cb,
                              const float *vp) {
    float rot[16], t[16], model[16], mvp[16], prev_mvp[16];
    mat4_look_rotation(rot, dx, dy, dz);
    mat4_translate(t, px, py, pz);
    mat4_mul(model, t, rot);
    mat4_mul(mvp, vp, model);
    /* Camera-motion-only scope, same reasoning as draw_box above — reuses
     * this frame's own model matrix for the "previous" reprojection. */
    mat4_mul(prev_mvp, r->prev_vp, model);

    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, prev_mvp);
    glUniform3f(r->u_mat_color, cr, cg, cb);
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    /* Safe on both backends now that wasm is WebGL2/GLES3, which has
     * glUniform1ui (GLES2/WebGL1 didn't — GLSL ES 1.00 has no uint type,
     * and this used to need a #ifndef __EMSCRIPTEN__ guard for exactly
     * that reason). */
    glUniform1ui(r->u_object_id, r->cur_object_id);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    int stride = 6 * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 0.0f);

    glDrawArrays(GL_TRIANGLES, 0, 36);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

static unsigned int s_player_vbo = 0;

void renderer_draw_players(Renderer *r, const GameState *gs, int local_id) {
    if (!s_player_vbo)
        build_box_vbo(&s_player_vbo, PLAYER_HALFWIDTH, PLAYER_HEIGHT, PLAYER_HALFWIDTH);
    float vp[16]; build_vp(r, vp);
    for (int i = 0; i < gs->num_players; i++) {
        const Player *p = &gs->players[i];
        if (!p->alive || p->id == (uint8_t)local_id) continue;
        float cr = p->is_bot ? 1.0f : 0.8f;
        float cg = p->is_bot ? 0.5f : 0.2f;
        float cb = p->is_bot ? 0.1f : 0.2f;
        r->cur_object_id = 1000u + p->id;
        draw_box(r, s_player_vbo, p->pos.x, p->pos.y, p->pos.z, cr, cg, cb, vp);
    }
}

void renderer_draw_rockets(Renderer *r, const GameState *gs) {
    float vp[16]; build_vp(r, vp);
    for (int i = 0; i < MAX_ROCKETS; i++) {
        const Rocket *rk = &gs->rockets[i];
        if (!rk->active) continue;
        r->cur_object_id = 2000u + (unsigned int)i;
        draw_box_oriented(r, r->rocket_vbo,
                 rk->pos.x, rk->pos.y, rk->pos.z,
                 rk->vel.x, rk->vel.y, rk->vel.z,
                 1.0f, 0.6f, 0.1f, vp);
    }
}

static unsigned int s_ground_vbo = 0;

void renderer_draw_ground_plane(Renderer *r) {
    if (!s_ground_vbo) {
        float y  = 15.5f;   /* just under y=16 so real floor geometry there always wins depth test */
        /* Exactly WORLD_SIZE, no margin — the highlight/editable area is
         * clamped to WORLD_SIZE too, so a padded plane would visually
         * extend past where anything can actually be targeted/built. */
        float x0 = 0.0f, x1 = (float)WORLD_SIZE;
        float z0 = 0.0f, z1 = (float)WORLD_SIZE;
        /* Same corner order as FACE_POS_Y quads in octree_render.c
         * (a=x0z0, b=x0z1, c=x1z1, d=x1z0; tris a,b,c and a,c,d) so winding
         * matches the engine's convention and isn't back-face culled. */
        float verts[6*6] = {
            x0,y,z0,  0,1,0,
            x0,y,z1,  0,1,0,
            x1,y,z1,  0,1,0,

            x0,y,z0,  0,1,0,
            x1,y,z1,  0,1,0,
            x1,y,z0,  0,1,0,
        };
        glGenBuffers(1, &s_ground_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, s_ground_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    }

    r->cur_object_id = 1;

    float vp[16]; build_vp(r, vp);
    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, vp);
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, r->prev_vp);  /* world space, no model matrix — see draw_world */
    glUniform3f(r->u_mat_color, 0.78f, 0.78f, 0.80f);   /* light grey */
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    /* Safe on both backends now that wasm is WebGL2/GLES3, which has
     * glUniform1ui (GLES2/WebGL1 didn't — GLSL ES 1.00 has no uint type,
     * and this used to need a #ifndef __EMSCRIPTEN__ guard for exactly
     * that reason). */
    glUniform1ui(r->u_object_id, r->cur_object_id);

    glBindBuffer(GL_ARRAY_BUFFER, s_ground_vbo);
    int stride = 6 * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 0.0f);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

static unsigned int s_wire_vbo = 0;

void renderer_draw_wire_box(Renderer *r, Vec3f bmin, Vec3f bmax,
                            float cr, float cg, float cb) {
    if (!s_wire_vbo) glGenBuffers(1, &s_wire_vbo);

    float c[8][3] = {
        {bmin.x,bmin.y,bmin.z}, {bmax.x,bmin.y,bmin.z},
        {bmax.x,bmin.y,bmax.z}, {bmin.x,bmin.y,bmax.z},
        {bmin.x,bmax.y,bmin.z}, {bmax.x,bmax.y,bmin.z},
        {bmax.x,bmax.y,bmax.z}, {bmin.x,bmax.y,bmax.z},
    };
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   /* bottom ring */
        {4,5},{5,6},{6,7},{7,4},   /* top ring */
        {0,4},{1,5},{2,6},{3,7},   /* verticals */
    };
    /* Normal == light dir for every vertex → full-bright regardless of
     * facing, so the highlight reads clearly from any angle. */
    float verts[12 * 2 * 6];
    float *vp_ = verts;
    for (int e = 0; e < 12; e++) {
        for (int k = 0; k < 2; k++) {
            const float *cp = c[edges[e][k]];
            *vp_++ = cp[0]; *vp_++ = cp[1]; *vp_++ = cp[2];
            *vp_++ = 0.577f; *vp_++ = 0.577f; *vp_++ = 0.577f;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_wire_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    r->cur_object_id = 3;

    float vp[16]; build_vp(r, vp);
    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, vp);
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, r->prev_vp);  /* world space, no model matrix — see draw_world */
    glUniform3f(r->u_mat_color, cr, cg, cb);
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    /* Safe on both backends now that wasm is WebGL2/GLES3, which has
     * glUniform1ui (GLES2/WebGL1 didn't — GLSL ES 1.00 has no uint type,
     * and this used to need a #ifndef __EMSCRIPTEN__ guard for exactly
     * that reason). */
    glUniform1ui(r->u_object_id, r->cur_object_id);

    int stride = 6 * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 0.0f);

    glDrawArrays(GL_LINES, 0, 24);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}
