#include "renderer.h"
#include "octree_render.h"
#include "meshobject.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>   /* offsetof -- renderer_draw_skinned_mesh's SkinnedVertex attrib pointers */

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
    /* Neutral dielectric/fully-rough default (metallic=0, roughness=1) --
     * this shared shader (world/ground/players/rockets) has no real PBR
     * material of its own yet, only a per-draw-call flat color. Matches a
     * plain Lambertian look under gbuffer.c's lighting pass now that it
     * actually reads this channel (see MeshObject's own PBR shader below
     * for the real, per-face version of this data). */
    "  out_material  = vec4(0.0, 1.0, 0.0, 0.0);\n"
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
    /* Neutral dielectric/fully-rough default (metallic=0, roughness=1) --
     * this shared shader (world/ground/players/rockets) has no real PBR
     * material of its own yet, only a per-draw-call flat color. Matches a
     * plain Lambertian look under gbuffer.c's lighting pass now that it
     * actually reads this channel (see MeshObject's own PBR shader below
     * for the real, per-face version of this data). */
    "  out_material  = vec4(0.0, 1.0, 0.0, 0.0);\n"
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

/* ---- MeshObject's own PBR shader (Phase 1's "PBR material assignment per
 * face") ----
 * Separate program from VERT_SRC/FRAG_SRC above -- see meshobject.h's
 * MESHOBJ_VERTEX_STRIDE comment for why this isn't just the shared program
 * with a bigger vertex format. Real per-face baseColor/metallic/roughness/
 * emission (as vertex attributes, duplicated per corner the same way
 * meshobject_build_render_mesh_from_halfedge already duplicates per-face
 * normals) flow straight into the G-buffer's albedo/material/emissive
 * outputs — this is what gbuffer.c's lighting pass now actually samples
 * (see its own comment) to produce a real, non-placeholder specular
 * response instead of the shared shader's neutral dielectric/rough
 * default. */
#ifdef __EMSCRIPTEN__
static const char *PBR_VERT_SRC =
    "#version 300 es\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "in vec3 a_base_color;\n"
    "in float a_metallic;\n"
    "in float a_roughness;\n"
    "in vec3 a_emission;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prev_mvp;\n"
    "out vec3 v_normal;\n"
    "out vec3 v_base_color;\n"
    "out float v_metallic;\n"
    "out float v_roughness;\n"
    "out vec3 v_emission;\n"
    "out vec3 v_clip_curr;\n"
    "out vec3 v_clip_prev;\n"
    "void main() {\n"
    "  vec4 clip = u_mvp * vec4(a_pos, 1.0);\n"
    "  gl_Position = clip;\n"
    "  v_clip_curr = vec3(clip.xy, clip.w);\n"
    "  vec4 clip_prev = u_prev_mvp * vec4(a_pos, 1.0);\n"
    "  v_clip_prev = vec3(clip_prev.xy, clip_prev.w);\n"
    "  v_normal = a_normal;\n"
    "  v_base_color = a_base_color;\n"
    "  v_metallic = a_metallic;\n"
    "  v_roughness = a_roughness;\n"
    "  v_emission = a_emission;\n"
    "}\n";

static const char *PBR_FRAG_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3  v_normal;\n"
    "in vec3  v_base_color;\n"
    "in float v_metallic;\n"
    "in float v_roughness;\n"
    "in vec3  v_emission;\n"
    "in vec3  v_clip_curr;\n"
    "in vec3  v_clip_prev;\n"
    "uniform uint  u_object_id;\n"
    "layout(location=0) out vec4 out_albedo;\n"
    "layout(location=1) out vec4 out_normal;\n"
    "layout(location=2) out vec4 out_material;\n"
    "layout(location=3) out vec4 out_emissive;\n"
    "layout(location=4) out vec4 out_velocity;\n"
    "layout(location=5) out uint out_object_id;\n"
    "void main() {\n"
    "  vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);\n"
    "  out_albedo    = vec4(v_base_color, 1.0);\n"
    "  out_normal    = vec4(n * 0.5 + 0.5, 0.0);\n"
    "  out_material  = vec4(v_metallic, v_roughness, 0.0, 0.0);\n"
    "  out_emissive  = vec4(v_emission, 0.0);\n"
    "  vec2 ndc_curr = v_clip_curr.xy / v_clip_curr.z;\n"
    "  vec2 ndc_prev = v_clip_prev.xy / v_clip_prev.z;\n"
    "  out_velocity  = vec4((ndc_curr - ndc_prev) * 0.5, 0.0, 0.0);\n"
    "  out_object_id = u_object_id;\n"
    "}\n";
#else
static const char *PBR_VERT_SRC =
    "#version 330 core\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "in vec3 a_base_color;\n"
    "in float a_metallic;\n"
    "in float a_roughness;\n"
    "in vec3 a_emission;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prev_mvp;\n"
    "out vec3 v_normal;\n"
    "out vec3 v_base_color;\n"
    "out float v_metallic;\n"
    "out float v_roughness;\n"
    "out vec3 v_emission;\n"
    "out vec3 v_clip_curr;\n"
    "out vec3 v_clip_prev;\n"
    "void main() {\n"
    "  vec4 clip = u_mvp * vec4(a_pos, 1.0);\n"
    "  gl_Position = clip;\n"
    "  v_clip_curr = vec3(clip.xy, clip.w);\n"
    "  vec4 clip_prev = u_prev_mvp * vec4(a_pos, 1.0);\n"
    "  v_clip_prev = vec3(clip_prev.xy, clip_prev.w);\n"
    "  v_normal = a_normal;\n"
    "  v_base_color = a_base_color;\n"
    "  v_metallic = a_metallic;\n"
    "  v_roughness = a_roughness;\n"
    "  v_emission = a_emission;\n"
    "}\n";

static const char *PBR_FRAG_SRC =
    "#version 330 core\n"
    "in vec3  v_normal;\n"
    "in vec3  v_base_color;\n"
    "in float v_metallic;\n"
    "in float v_roughness;\n"
    "in vec3  v_emission;\n"
    "in vec3  v_clip_curr;\n"
    "in vec3  v_clip_prev;\n"
    "uniform uint  u_object_id;\n"
    "layout(location=0) out vec4 out_albedo;\n"
    "layout(location=1) out vec4 out_normal;\n"
    "layout(location=2) out vec4 out_material;\n"
    "layout(location=3) out vec4 out_emissive;\n"
    "layout(location=4) out vec4 out_velocity;\n"
    "layout(location=5) out uint out_object_id;\n"
    "void main() {\n"
    "  vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);\n"
    "  out_albedo    = vec4(v_base_color, 1.0);\n"
    "  out_normal    = vec4(n * 0.5 + 0.5, 0.0);\n"
    "  out_material  = vec4(v_metallic, v_roughness, 0.0, 0.0);\n"
    "  out_emissive  = vec4(v_emission, 0.0);\n"
    "  vec2 ndc_curr = v_clip_curr.xy / v_clip_curr.z;\n"
    "  vec2 ndc_prev = v_clip_prev.xy / v_clip_prev.z;\n"
    "  out_velocity  = vec4((ndc_curr - ndc_prev) * 0.5, 0.0, 0.0);\n"
    "  out_object_id = u_object_id;\n"
    "}\n";
#endif

/* Phase 4's GPU vertex-skinning shader (see renderer_draw_skinned_mesh,
 * skinned_mesh_object.h) -- %d is SKINNED_SHADER_MAX_BONES, substituted
 * at link time (link_skinned_program) rather than hardcoded twice, so
 * the array-size literal in GLSL can never drift out of sync with the
 * C-side upload code's own bound. a_bone_idx arrives as raw unnormalized
 * GL_UNSIGNED_BYTE values (see renderer_draw_skinned_mesh's
 * glVertexAttribPointer call) -- read here as plain floats (0..255) and
 * cast to int for the u_bones[] index, exactly SkinnedVertex::bone_idx's
 * own uint8_t values, no remapping needed. Normal transform uses only
 * the skin matrix's rotational 3x3 part (mat3(skin)) -- the standard
 * real-time-skinning simplification (ignores the inverse-transpose
 * correction a non-uniformly-scaled skin matrix would strictly need),
 * which every mainstream real-time engine's basic GPU skinning path
 * also uses, not a shortcut invented here. */
#ifdef __EMSCRIPTEN__
static const char *SKINNED_VERT_SRC_FMT =
    "#version 300 es\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "in vec4 a_bone_idx;\n"
    "in vec4 a_bone_wgt;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prev_mvp;\n"
    "uniform mat4 u_bones[%d];\n"
    "out vec3 v_normal;\n"
    "out vec3 v_clip_curr;\n"
    "out vec3 v_clip_prev;\n"
    "void main() {\n"
    "  mat4 skin = a_bone_wgt.x * u_bones[int(a_bone_idx.x)]\n"
    "            + a_bone_wgt.y * u_bones[int(a_bone_idx.y)]\n"
    "            + a_bone_wgt.z * u_bones[int(a_bone_idx.z)]\n"
    "            + a_bone_wgt.w * u_bones[int(a_bone_idx.w)];\n"
    "  vec4 skinned_pos = skin * vec4(a_pos, 1.0);\n"
    "  vec4 clip = u_mvp * skinned_pos;\n"
    "  gl_Position = clip;\n"
    "  v_clip_curr = vec3(clip.xy, clip.w);\n"
    "  vec4 clip_prev = u_prev_mvp * skinned_pos;\n"
    "  v_clip_prev = vec3(clip_prev.xy, clip_prev.w);\n"
    "  v_normal = mat3(skin) * a_normal;\n"
    "}\n";

static const char *SKINNED_FRAG_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_normal;\n"
    "in vec3 v_clip_curr;\n"
    "in vec3 v_clip_prev;\n"
    "uniform vec3  u_base_color;\n"
    "uniform float u_metallic;\n"
    "uniform float u_roughness;\n"
    "uniform vec3  u_emission;\n"
    "uniform uint  u_object_id;\n"
    "layout(location=0) out vec4 out_albedo;\n"
    "layout(location=1) out vec4 out_normal;\n"
    "layout(location=2) out vec4 out_material;\n"
    "layout(location=3) out vec4 out_emissive;\n"
    "layout(location=4) out vec4 out_velocity;\n"
    "layout(location=5) out uint out_object_id;\n"
    "void main() {\n"
    "  vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);\n"
    "  out_albedo    = vec4(u_base_color, 1.0);\n"
    "  out_normal    = vec4(n * 0.5 + 0.5, 0.0);\n"
    "  out_material  = vec4(u_metallic, u_roughness, 0.0, 0.0);\n"
    "  out_emissive  = vec4(u_emission, 0.0);\n"
    "  vec2 ndc_curr = v_clip_curr.xy / v_clip_curr.z;\n"
    "  vec2 ndc_prev = v_clip_prev.xy / v_clip_prev.z;\n"
    "  out_velocity  = vec4((ndc_curr - ndc_prev) * 0.5, 0.0, 0.0);\n"
    "  out_object_id = u_object_id;\n"
    "}\n";
#else
static const char *SKINNED_VERT_SRC_FMT =
    "#version 330 core\n"
    "in vec3 a_pos;\n"
    "in vec3 a_normal;\n"
    "in vec4 a_bone_idx;\n"
    "in vec4 a_bone_wgt;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_prev_mvp;\n"
    "uniform mat4 u_bones[%d];\n"
    "out vec3 v_normal;\n"
    "out vec3 v_clip_curr;\n"
    "out vec3 v_clip_prev;\n"
    "void main() {\n"
    "  mat4 skin = a_bone_wgt.x * u_bones[int(a_bone_idx.x)]\n"
    "            + a_bone_wgt.y * u_bones[int(a_bone_idx.y)]\n"
    "            + a_bone_wgt.z * u_bones[int(a_bone_idx.z)]\n"
    "            + a_bone_wgt.w * u_bones[int(a_bone_idx.w)];\n"
    "  vec4 skinned_pos = skin * vec4(a_pos, 1.0);\n"
    "  vec4 clip = u_mvp * skinned_pos;\n"
    "  gl_Position = clip;\n"
    "  v_clip_curr = vec3(clip.xy, clip.w);\n"
    "  vec4 clip_prev = u_prev_mvp * skinned_pos;\n"
    "  v_clip_prev = vec3(clip_prev.xy, clip_prev.w);\n"
    "  v_normal = mat3(skin) * a_normal;\n"
    "}\n";

static const char *SKINNED_FRAG_SRC =
    "#version 330 core\n"
    "in vec3 v_normal;\n"
    "in vec3 v_clip_curr;\n"
    "in vec3 v_clip_prev;\n"
    "uniform vec3  u_base_color;\n"
    "uniform float u_metallic;\n"
    "uniform float u_roughness;\n"
    "uniform vec3  u_emission;\n"
    "uniform uint  u_object_id;\n"
    "layout(location=0) out vec4 out_albedo;\n"
    "layout(location=1) out vec4 out_normal;\n"
    "layout(location=2) out vec4 out_material;\n"
    "layout(location=3) out vec4 out_emissive;\n"
    "layout(location=4) out vec4 out_velocity;\n"
    "layout(location=5) out uint out_object_id;\n"
    "void main() {\n"
    "  vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);\n"
    "  out_albedo    = vec4(u_base_color, 1.0);\n"
    "  out_normal    = vec4(n * 0.5 + 0.5, 0.0);\n"
    "  out_material  = vec4(u_metallic, u_roughness, 0.0, 0.0);\n"
    "  out_emissive  = vec4(u_emission, 0.0);\n"
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
 * path no longer relies on glClearColor's implicit background-fill.
 * Dark, dark grey per an explicit request -- the original (0.3,0.5,0.8)
 * blue was a literal Qek holdover (that project's actual sky color),
 * never revisited since. */
static float s_sky_color[3] = {0.06f, 0.06f, 0.06f};

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

/* Non-uniform scale -- MeshObject::scale (see meshobject.h), driven by
 * main.c's S transform tool. Applied first in the model matrix (T*R*S,
 * the standard TRS order: scale in local space, then rotate, then
 * translate into world space) via renderer_draw_mesh_object below. */
static void mat4_scale(float *m, float sx, float sy, float sz) {
    mat4_identity(m);
    m[0] = sx; m[5] = sy; m[10] = sz;
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

/* Same shape as link_program, but binds MeshObject's own PBR attribute set
 * (see PBR_VERT_SRC/PBR_FRAG_SRC's own comment) instead of a_pos/a_normal/
 * a_mat_id -- kept as a separate function rather than parameterizing
 * link_program's attribute list, since this is the only other program
 * this renderer ever links and a one-off list doesn't earn a shared
 * general mechanism. */
static unsigned int link_pbr_program(const char *vsrc, const char *fsrc) {
    unsigned int vs = compile_shader(GL_VERTEX_SHADER,   vsrc);
    unsigned int fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    unsigned int p  = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_normal");
    glBindAttribLocation(p, 2, "a_base_color");
    glBindAttribLocation(p, 3, "a_metallic");
    glBindAttribLocation(p, 4, "a_roughness");
    glBindAttribLocation(p, 5, "a_emission");
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, NULL, log);
        printf("[renderer] PBR program link error: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    gl_check("link_pbr_program");
    return p;
}

/* Same shape again, for Phase 4's skinned-mesh program -- formats
 * SKINNED_VERT_SRC_FMT's "%d" bone-count placeholder with SKINNED_
 * SHADER_MAX_BONES first (see that macro's own comment on why this is
 * templated rather than hardcoded in the GLSL source twice). */
static unsigned int link_skinned_program(const char *vsrc_fmt, const char *fsrc) {
    char vsrc[4096];
    snprintf(vsrc, sizeof(vsrc), vsrc_fmt, SKINNED_SHADER_MAX_BONES);
    unsigned int vs = compile_shader(GL_VERTEX_SHADER,   vsrc);
    unsigned int fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    unsigned int p  = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_normal");
    glBindAttribLocation(p, 2, "a_bone_idx");
    glBindAttribLocation(p, 3, "a_bone_wgt");
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, NULL, log);
        printf("[renderer] Skinned program link error: %s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    gl_check("link_skinned_program");
    return p;
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
     * behind/inside geometry and player boxes (impossible in normal
     * collided play), so both winding directions of every triangle must
     * rasterize or those faces just vanish from certain angles. */
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

    r->pbr_program    = link_pbr_program(PBR_VERT_SRC, PBR_FRAG_SRC);
    r->pbr_u_mvp      = glGetUniformLocation(r->pbr_program, "u_mvp");
    r->pbr_u_prev_mvp = glGetUniformLocation(r->pbr_program, "u_prev_mvp");
    r->pbr_u_object_id = glGetUniformLocation(r->pbr_program, "u_object_id");
    printf("[renderer] pbr_prog=%u pbr_mvp=%d pbr_prev_mvp=%d\n",
           r->pbr_program, r->pbr_u_mvp, r->pbr_u_prev_mvp);

    r->skinned_program      = link_skinned_program(SKINNED_VERT_SRC_FMT, SKINNED_FRAG_SRC);
    r->skinned_u_mvp        = glGetUniformLocation(r->skinned_program, "u_mvp");
    r->skinned_u_prev_mvp   = glGetUniformLocation(r->skinned_program, "u_prev_mvp");
    r->skinned_u_object_id  = glGetUniformLocation(r->skinned_program, "u_object_id");
    r->skinned_u_base_color = glGetUniformLocation(r->skinned_program, "u_base_color");
    r->skinned_u_metallic   = glGetUniformLocation(r->skinned_program, "u_metallic");
    r->skinned_u_roughness  = glGetUniformLocation(r->skinned_program, "u_roughness");
    r->skinned_u_emission   = glGetUniformLocation(r->skinned_program, "u_emission");
    r->skinned_u_bones      = glGetUniformLocation(r->skinned_program, "u_bones");
    printf("[renderer] skinned_prog=%u skinned_mvp=%d skinned_bones=%d\n",
           r->skinned_program, r->skinned_u_mvp, r->skinned_u_bones);

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
    glDeleteProgram(r->pbr_program);
    glDeleteProgram(r->skinned_program);
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

void renderer_set_camera(Renderer *r, Vec3f eye, float yaw, float pitch) {
    r->cam_pos[0] = eye.x;
    r->cam_pos[1] = eye.y;
    r->cam_pos[2] = eye.z;
    r->cam_yaw    = yaw;
    r->cam_pitch  = pitch;
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

/* Phase 1 foundation: draws a MeshObject (glTF-sourced, via the half-edge
 * structure — see meshobject.h/halfedge_gltf.c) with its own position/
 * orientation model transform, through its OWN PBR shader/program (see
 * PBR_VERT_SRC/PBR_FRAG_SRC and meshobject.h's MESHOBJ_VERTEX_STRIDE) —
 * not the shared world/ground/players/rockets program, since only this
 * entity type carries real per-face material data. */
void renderer_draw_mesh_object(Renderer *r, const MeshObject *obj) {
    if (!obj->render_mesh || obj->render_mesh->count == 0) return;

    mesh_upload_stride(obj->render_mesh, MESHOBJ_VERTEX_STRIDE);
    if (!obj->render_mesh->vbo) return;

    float vp[16]; build_vp(r, vp);
    float rot[16], t[16], s[16], rs[16], model[16], mvp[16], prev_mvp[16];
    quat_to_mat4(&obj->orientation, rot);
    mat4_translate(t, obj->position.x, obj->position.y, obj->position.z);
    mat4_scale(s, obj->scale.x, obj->scale.y, obj->scale.z);
    mat4_mul(rs, rot, s);
    mat4_mul(model, t, rs);
    mat4_mul(mvp, vp, model);
    /* Camera-motion-only velocity scope, same reasoning as draw_box/
     * draw_box_oriented — a static MeshObject's own transform doesn't
     * change frame to frame anyway (is_static), so this is exact for
     * static objects specifically, not just an approximation. */
    mat4_mul(prev_mvp, r->prev_vp, model);

    r->cur_object_id = 4000u + (unsigned int)obj->id;

    glUseProgram(r->pbr_program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->pbr_u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(r->pbr_u_prev_mvp, 1, GL_FALSE, prev_mvp);
    glUniform1ui(r->pbr_u_object_id, r->cur_object_id);

    glBindBuffer(GL_ARRAY_BUFFER, obj->render_mesh->vbo);
    int stride = MESHOBJ_VERTEX_STRIDE * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(9*sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)(10*sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, stride, (void*)(11*sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, obj->render_mesh->count);
    gl_check("draw_mesh_object");

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
}

/* Phase 4's real GPU vertex skinning (see skinned_mesh_object.h,
 * renderer.h's own comment on this function). Real EBO-indexed draw
 * (glDrawElements) -- unlike MeshObject's non-indexed/vertex-duplicated
 * convention, SkinnedMesh keeps its glTF-native shared-vertex index
 * buffer (skinned_mesh.h), a better fit for skinning (shared vertices
 * carry a single set of bone weights each, not duplicated per triangle). */
void renderer_draw_skinned_mesh(Renderer *r, SkinnedMeshObject *obj, unsigned int object_id) {
    if (!obj->mesh.index_count) return;

    if (!obj->gpu_uploaded) {
        glGenBuffers(1, &obj->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, obj->vbo);
        glBufferData(GL_ARRAY_BUFFER, (long)obj->mesh.vert_count * (long)sizeof(SkinnedVertex),
                     obj->mesh.verts, GL_STATIC_DRAW);
        glGenBuffers(1, &obj->ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long)obj->mesh.index_count * (long)sizeof(uint16_t),
                     obj->mesh.indices, GL_STATIC_DRAW);
        obj->gpu_uploaded = 1;
        gl_check("renderer_draw_skinned_mesh upload");
    }

    float vp[16]; build_vp(r, vp);
    float rot[16], t[16], s[16], rs[16], model[16], mvp[16], prev_mvp[16];
    quat_to_mat4(&obj->orientation, rot);
    mat4_translate(t, obj->position.x, obj->position.y, obj->position.z);
    mat4_scale(s, obj->scale.x, obj->scale.y, obj->scale.z);
    mat4_mul(rs, rot, s);
    mat4_mul(model, t, rs);
    mat4_mul(mvp, vp, model);
    mat4_mul(prev_mvp, r->prev_vp, model);   /* camera-motion-only, same scope note as renderer_draw_mesh_object */

    r->cur_object_id = object_id;

    glUseProgram(r->skinned_program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->skinned_u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(r->skinned_u_prev_mvp, 1, GL_FALSE, prev_mvp);
    glUniform1ui(r->skinned_u_object_id, r->cur_object_id);
    glUniform3f(r->skinned_u_base_color, obj->base_color.x, obj->base_color.y, obj->base_color.z);
    glUniform1f(r->skinned_u_metallic, obj->metallic);
    glUniform1f(r->skinned_u_roughness, obj->roughness);
    glUniform3f(r->skinned_u_emission, obj->emission.x, obj->emission.y, obj->emission.z);
    int n_bones = obj->arm.bone_count < SKINNED_SHADER_MAX_BONES ? obj->arm.bone_count : SKINNED_SHADER_MAX_BONES;
    glUniformMatrix4fv(r->skinned_u_bones, n_bones, GL_FALSE, &obj->skin[0][0]);

    glBindBuffer(GL_ARRAY_BUFFER, obj->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->ebo);
    int stride = (int)sizeof(SkinnedVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SkinnedVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SkinnedVertex, normal));
    glEnableVertexAttribArray(2);
    /* Raw, UNNORMALIZED unsigned bytes -- arrives in the shader as plain
     * floats holding the byte's own integer value (5 -> 5.0), exactly
     * what SKINNED_VERT_SRC_FMT's int(a_bone_idx.x) expects; GL_TRUE
     * here would instead rescale to [0,1], which is NOT what a bone
     * index needs. */
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, (void*)offsetof(SkinnedVertex, bone_idx));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SkinnedVertex, bone_wgt));

    glDrawElements(GL_TRIANGLES, obj->mesh.index_count, GL_UNSIGNED_SHORT, (void*)0);
    gl_check("draw_skinned_mesh");

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

/* ---- Vector helpers (file-static, matching gizmo.c's/light.c's own
 * vec3_* naming convention rather than a shared header) -- only needed
 * by renderer_draw_lights below. ---- */
static inline Vec3f vec3_add(Vec3f a, Vec3f b) { return (Vec3f){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3f vec3_scale(Vec3f a, float s) { return (Vec3f){a.x*s, a.y*s, a.z*s}; }
static inline float vec3_dot(Vec3f a, Vec3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float vec3_len(Vec3f v) { return sqrtf(vec3_dot(v, v)); }
static inline Vec3f vec3_norm(Vec3f v) {
    float l = vec3_len(v);
    if (l < 1e-6f) return (Vec3f){0,0,0};
    return vec3_scale(v, 1.0f / l);
}

static void light_icon_color_for_type(LightType type, float *r, float *g, float *b) {
    switch (type) {
        case LIGHT_TYPE_POINT: *r = 1.0f; *g = 0.85f; *b = 0.3f; break;   /* warm yellow */
        case LIGHT_TYPE_SUN:   *r = 1.0f; *g = 0.95f; *b = 0.8f; break;   /* near-white */
        case LIGHT_TYPE_SPOT:  *r = 0.4f; *g = 0.8f;  *b = 1.0f; break;   /* cyan-ish */
        case LIGHT_TYPE_AREA:  *r = 0.85f; *g = 0.5f; *b = 1.0f; break;   /* violet */
        default:                *r = 1.0f; *g = 1.0f; *b = 1.0f; break;
    }
}

void renderer_draw_lights(Renderer *r) {
    PhiLight *lights[PHI_MAX_LIGHTS];
    int n = light_get_all(lights);
    for (int i = 0; i < n; i++) {
        const PhiLight *l = lights[i];
        float cr, cg, cb;
        light_icon_color_for_type(l->type, &cr, &cg, &cb);

        float ir = LIGHT_ICON_RADIUS * 0.5f;
        Vec3f bmin = { l->position.x - ir, l->position.y - ir, l->position.z - ir };
        Vec3f bmax = { l->position.x + ir, l->position.y + ir, l->position.z + ir };
        renderer_draw_solid_box(r, bmin, bmax, cr, cg, cb);

        if (l->type == LIGHT_TYPE_SUN || l->type == LIGHT_TYPE_SPOT) {
            /* A short thin box from the light toward `direction` -- the
             * same "solid geometry, not GL_LINES" rule this project's
             * grid/gizmo already established (1px lines get suppressed
             * by this deferred pipeline's TAA/FXAA). */
            Vec3f dir = vec3_norm(l->direction);
            if (vec3_len(dir) > 0.0f) {
                Vec3f tip = vec3_add(l->position, vec3_scale(dir, LIGHT_ICON_RADIUS * 3.0f));
                float pad = ir * 0.3f;
                Vec3f smin = {
                    fminf(l->position.x, tip.x) - pad, fminf(l->position.y, tip.y) - pad, fminf(l->position.z, tip.z) - pad
                };
                Vec3f smax = {
                    fmaxf(l->position.x, tip.x) + pad, fmaxf(l->position.y, tip.y) + pad, fmaxf(l->position.z, tip.z) + pad
                };
                renderer_draw_solid_box(r, smin, smax, cr, cg, cb);
            }
        }
    }
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

static unsigned int s_solid_box_vbo = 0;

/* Solid (filled-triangle) box, e.g. the transform gizmo's shaft/handles
 * (gizmo.c) — renderer_draw_wire_box's 1-pixel GL_LINES edges turned out
 * to be too thin to survive this pipeline's TAA/FXAA: the geometry pass
 * genuinely wrote the right object_id (confirmed via a direct G-buffer
 * readback), but the final composited frame didn't show it, since a
 * sub-pixel-coverage line is exactly the kind of high-frequency detail
 * TAA's temporal blending and FXAA's edge-smoothing are designed to
 * suppress. A filled box has real per-pixel area for those passes to
 * accumulate, so it survives. Same shader/VBO-management pattern as
 * renderer_draw_wire_box just above — only the topology (12 triangles
 * instead of 12 line edges) and draw mode differ. */
void renderer_draw_solid_box(Renderer *r, Vec3f bmin, Vec3f bmax,
                              float cr, float cg, float cb) {
    if (!s_solid_box_vbo) glGenBuffers(1, &s_solid_box_vbo);

    float c[8][3] = {
        {bmin.x,bmin.y,bmin.z}, {bmax.x,bmin.y,bmin.z},
        {bmax.x,bmin.y,bmax.z}, {bmin.x,bmin.y,bmax.z},
        {bmin.x,bmax.y,bmin.z}, {bmax.x,bmax.y,bmin.z},
        {bmax.x,bmax.y,bmax.z}, {bmin.x,bmax.y,bmax.z},
    };
    /* Two triangles per face, 6 faces — winding doesn't matter for
     * visibility here (same "normal == light dir always" full-bright
     * trick as wire_box means every face is uniformly lit regardless of
     * facing/winding, see the vertex loop below). */
    static const int faces[6][4] = {
        {0,1,2,3}, {5,4,7,6},   /* bottom, top */
        {4,0,3,7}, {1,5,6,2},   /* left, right */
        {4,5,1,0}, {3,2,6,7},   /* front, back */
    };
    float verts[6 * 6 * 6];  /* 6 faces * 6 verts/face (2 tris) * 6 floats/vert */
    float *vp_ = verts;
    for (int f = 0; f < 6; f++) {
        int quad[6] = { faces[f][0], faces[f][1], faces[f][2], faces[f][0], faces[f][2], faces[f][3] };
        for (int k = 0; k < 6; k++) {
            const float *cp = c[quad[k]];
            *vp_++ = cp[0]; *vp_++ = cp[1]; *vp_++ = cp[2];
            *vp_++ = 0.577f; *vp_++ = 0.577f; *vp_++ = 0.577f;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_solid_box_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    r->cur_object_id = 3;

    float vp[16]; build_vp(r, vp);
    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, vp);
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, r->prev_vp);
    glUniform3f(r->u_mat_color, cr, cg, cb);
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    glUniform1ui(r->u_object_id, r->cur_object_id);

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

static unsigned int s_grid_vbo = 0;
static int          s_grid_vert_count = 0;

/* Reference grid on the XZ plane -- built from thin SOLID quads (two
 * triangles per line), same "full-bright normal == light dir" trick and
 * the same r->program/u_mvp/u_mat_color/u_object_id shape every helper-
 * geometry draw in this file uses, deliberately NOT renderer_draw_wire_
 * box's GL_LINES approach: that function's own comment (and gizmo.c's,
 * which switched away from it for exactly this reason) documents that a
 * 1-pixel line can genuinely rasterize in the geometry pass and still
 * vanish from the final composited frame, suppressed by TAA/FXAA's
 * temporal/edge-smoothing passes -- a real, previously-hit bug in this
 * codebase, not a hypothetical one, so the grid doesn't repeat it. */
void renderer_draw_grid(Renderer *r, Vec3f center, float half_extent,
                         float spacing, float line_width,
                         float cr, float cg, float cb) {
    if (!s_grid_vbo) {
        glGenBuffers(1, &s_grid_vbo);

        int n = (int)(half_extent / spacing);
        int line_count = 2 * (2 * n + 1);   /* n lines each side of center, plus the center line, in both directions */
        int floats_per_line = 6 * 6;        /* 2 triangles * 3 verts * (pos3 + normal3) */
        float *verts = (float *)malloc((size_t)line_count * (size_t)floats_per_line * sizeof(float));
        float *vp_ = verts;
        float hw = line_width * 0.5f;
        float z0 = center.z - half_extent, z1 = center.z + half_extent;
        float x0 = center.x - half_extent, x1 = center.x + half_extent;

        for (int i = -n; i <= n; i++) {
            /* Line running along Z at fixed x */
            float x = center.x + (float)i * spacing;
            float qx[4][3] = {
                {x - hw, center.y, z0}, {x + hw, center.y, z0},
                {x + hw, center.y, z1}, {x - hw, center.y, z1},
            };
            int tri[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) {
                const float *cp = qx[tri[k]];
                *vp_++ = cp[0]; *vp_++ = cp[1]; *vp_++ = cp[2];
                *vp_++ = 0.577f; *vp_++ = 0.577f; *vp_++ = 0.577f;
            }
            /* Line running along X at fixed z */
            float z = center.z + (float)i * spacing;
            float qz[4][3] = {
                {x0, center.y, z - hw}, {x1, center.y, z - hw},
                {x1, center.y, z + hw}, {x0, center.y, z + hw},
            };
            for (int k = 0; k < 6; k++) {
                const float *cp = qz[tri[k]];
                *vp_++ = cp[0]; *vp_++ = cp[1]; *vp_++ = cp[2];
                *vp_++ = 0.577f; *vp_++ = 0.577f; *vp_++ = 0.577f;
            }
        }

        s_grid_vert_count = line_count * 6;
        glBindBuffer(GL_ARRAY_BUFFER, s_grid_vbo);
        glBufferData(GL_ARRAY_BUFFER, (long)((size_t)line_count * (size_t)floats_per_line * sizeof(float)), verts, GL_STATIC_DRAW);
        free(verts);
    }

    r->cur_object_id = 0;   /* non-pickable world geometry, same convention as renderer.c's own default */

    float vp[16]; build_vp(r, vp);
    glUseProgram(r->program);
    bind_renderer_vao(r);
    glUniformMatrix4fv(r->u_mvp, 1, GL_FALSE, vp);
    glUniformMatrix4fv(r->u_prev_mvp, 1, GL_FALSE, r->prev_vp);
    glUniform3f(r->u_mat_color, cr, cg, cb);
    float ld[3] = {0.577f, 0.577f, 0.577f};
    glUniform3fv(r->u_light_dir, 1, ld);
    glUniform1ui(r->u_object_id, r->cur_object_id);

    glBindBuffer(GL_ARRAY_BUFFER, s_grid_vbo);
    int stride = 6 * (int)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3*sizeof(float)));
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 0.0f);

    glDrawArrays(GL_TRIANGLES, 0, s_grid_vert_count);
    gl_check("draw_grid");

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}
