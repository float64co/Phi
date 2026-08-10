#pragma once
#include "vec3.h"
#include "octree_render.h"
#include "meshobject.h"

typedef struct {
    /* WebGL program */
    unsigned int program;
    unsigned int vao;       /* re-bound before every draw, see bind_renderer_vao in renderer.c */

    /* Uniforms */
    int u_mvp;
    int u_prev_mvp;   /* see prev_vp below — for the geometry pass's
                        * velocity-buffer output (TAA) */
    int u_normal_mat;
    int u_light_dir;
    int u_mat_color;
    int u_object_id;   /* native G-buffer geometry pass only; -1 (safe no-op
                         * uniform location) on wasm, whose shader doesn't
                         * declare it */

    /* Set by each draw_* call before drawing (currently just
     * MeshObjects=4000+id, and gizmo.c's own wire-box id=3 set directly by
     * the caller via renderer_set_object_id) — read by the geometry-pass
     * shader's u_object_id uniform. Meaningless on wasm (see above). The
     * world=0/ground=1/players=1000+id/rockets=2000+slot conventions this
     * comment used to also list belonged to Qek's octree world/gameplay
     * draw calls, removed along with the rest of that code (see phi.md's
     * Phase 1 status) -- those object-id ranges are simply unused now,
     * not reassigned to anything. */
    unsigned int cur_object_id;

    /* Attribute locations */
    int a_pos;
    int a_normal;
    int a_mat_id;

    /* MeshObject's own PBR shader/program (see meshobject.h's
     * MESHOBJ_VERTEX_STRIDE) -- a separate program from the shared one
     * above rather than growing it for everyone, see meshobject.h's own
     * comment on why. Attribute locations (0=a_pos, 1=a_normal,
     * 2=a_base_color, 3=a_metallic, 4=a_roughness, 5=a_emission) are bound
     * in link_pbr_program and only ever used by renderer_draw_mesh_object,
     * so unlike a_pos/a_normal/a_mat_id above they don't need their own
     * Renderer fields -- the one draw call that uses them just hardcodes
     * the same fixed locations link_pbr_program bound. */
    unsigned int pbr_program;
    int pbr_u_mvp;
    int pbr_u_prev_mvp;
    int pbr_u_object_id;

    /* Camera */
    float cam_pos[3];
    float cam_yaw;
    float cam_pitch;
    float fov_y;
    int   vp_w, vp_h;

    /* Previous frame's view-projection matrix, snapshotted by
     * renderer_end_frame() — every draw_* call multiplies its own (current-
     * frame) model matrix against both this and the current vp to give the
     * geometry pass's vertex shader curr/prev clip positions for the
     * velocity buffer. Scoped to camera motion only: each draw reuses its
     * own CURRENT model matrix for the "previous" reprojection too (no
     * per-object previous-transform tracking), so a genuinely fast-moving
     * object's own motion isn't captured, only the parallax from
     * camera movement — see gbuffer.h's TAA comment for the honest caveat
     * this implies (mild ghosting/blur on fast movers specifically). */
    float prev_vp[16];
} Renderer;

Renderer *renderer_create(int width, int height);
void      renderer_destroy(Renderer *r);
void      renderer_resize(Renderer *r, int w, int h);

/* Console 'fov'/'skybox' commands */
void renderer_set_fov(Renderer *r, float degrees);
void renderer_set_sky_color(float r, float g, float b);
void renderer_get_sky_color(float *out3);  /* out3[0..2] = r,g,b — see gbuffer.c's lighting pass */

/* Sets the camera directly from a world-space eye position and yaw/pitch
 * (radians) -- previously read these off a Player struct
 * (renderer_set_camera(Renderer*, const Player*)), which no longer exists
 * now that Qek's gameplay/Player code is gone (see phi.md's Phase 1
 * status). main.c owns a minimal standalone camera state now instead. */
void renderer_set_camera(Renderer *r, Vec3f eye, float yaw, float pitch);

/* Snapshots this frame's view-projection matrix into prev_vp, for the
 * NEXT frame's velocity-buffer computation. Call once per frame, after
 * every draw_* call for the frame is done (main.c does this right after
 * editor_render, before the shadow/resolve passes — order relative to
 * those doesn't matter, this only touches Renderer's own state). */
void renderer_end_frame(Renderer *r);

/* Inverse of the current camera view-projection matrix — native only
 * (used by gbuffer.c's lighting pass to reconstruct world-space position
 * from G-buffer depth, e.g. for shadow-map sampling). Returns 1 on
 * success, 0 if the matrix was singular (shouldn't happen for a valid
 * camera, but checked rather than assumed). */
int renderer_get_inverse_view_proj(const Renderer *r, float *out16);

/* For future picking/editor use (Phase 1) — the built-in draw_* calls
 * already set sensible per-object ids internally; this is for callers
 * that want to override that. No-op on wasm (see Renderer.u_object_id). */
void renderer_set_object_id(Renderer *r, unsigned int id);

/* Phase 1 foundation: draw a single MeshObject at its own position/
 * orientation transform (see meshobject.h). */
void renderer_draw_mesh_object(Renderer *r, const MeshObject *obj);

/* Editor: draw a full-bright wireframe box in world space (hover/selection highlight) */
void renderer_draw_wire_box(Renderer *r, Vec3f bmin, Vec3f bmax,
                            float cr, float cg, float cb);

/* Editor: draw a full-bright SOLID (filled-triangle) box in world space —
 * e.g. the transform gizmo (gizmo.c). Prefer this over renderer_draw_wire_box
 * for anything that needs to reliably survive TAA/FXAA — a 1-pixel
 * wireframe line can vanish in the final composited frame even though the
 * geometry pass genuinely rasterized it (see gizmo.c's own note on this). */
void renderer_draw_solid_box(Renderer *r, Vec3f bmin, Vec3f bmax,
                              float cr, float cg, float cb);

/* Editor: a flat reference grid on the XZ plane, built from thin SOLID
 * quads (not GL_LINES) for the same TAA/FXAA-survival reason
 * renderer_draw_solid_box exists -- a real ground/spacing reference for
 * orbit/pan navigation. Built once and cached internally (a real static
 * VBO, not rebuilt every frame the way renderer_draw_wire_box's dynamic
 * per-call buffer is) since main.c only ever calls this with one fixed
 * center/extent/spacing describing the world's own ground plane; the
 * first call's arguments are the ones that stick. */
void renderer_draw_grid(Renderer *r, Vec3f center, float half_extent,
                         float spacing, float line_width,
                         float cr, float cg, float cb);

/* Math helpers exposed for main.c */
void mat4_perspective(float *m, float fovy, float aspect, float near, float far);
void mat4_mul(float *out, const float *a, const float *b);
void mat4_identity(float *m);
