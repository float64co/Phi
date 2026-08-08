#pragma once
#include "octree.h"
#include "octree_render.h"
#include "physics.h"

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

    /* Set by each draw_* call before drawing (world=0, ground=1,
     * players=1000+id, rockets=2000+slot) — read by the geometry-pass
     * shader's u_object_id uniform. Meaningless on wasm (see above). */
    unsigned int cur_object_id;

    /* Attribute locations */
    int a_pos;
    int a_normal;
    int a_mat_id;

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
     * player/rocket's own motion isn't captured, only the parallax from
     * camera movement — see gbuffer.h's TAA comment for the honest caveat
     * this implies (mild ghosting/blur on fast movers specifically). */
    float prev_vp[16];

    /* Rocket billboard mesh */
    unsigned int rocket_vbo;

    /* Flat-shade color palette (one vec3 per material id 0..255) */
    float palette[256][3];
} Renderer;

Renderer *renderer_create(int width, int height);
void      renderer_destroy(Renderer *r);
void      renderer_resize(Renderer *r, int w, int h);

/* Console 'fov'/'skybox' commands */
void renderer_set_fov(Renderer *r, float degrees);
void renderer_set_sky_color(float r, float g, float b);
void renderer_get_sky_color(float *out3);  /* out3[0..2] = r,g,b — see gbuffer.c's lighting pass */

/* Set camera from local player */
void renderer_set_camera(Renderer *r, const Player *p);

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

/* Draw world mesh */
void renderer_draw_world(Renderer *r, RenderMesh *mesh);

/* Draw all players (simple box) */
void renderer_draw_players(Renderer *r, const GameState *gs, int local_id);

/* Draw all active rockets */
void renderer_draw_rockets(Renderer *r, const GameState *gs);

/* Light-grey reference floor at y=15.5, spanning the world footprint —
 * physics.c hard-clamps every player to y>=16 regardless of octree content,
 * so this keeps that implicit ground visible/paintable even where the
 * octree itself has been carved fully empty. Depth-tests normally, so real
 * (carved/built) geometry at y=16 always draws over it. */
void renderer_draw_ground_plane(Renderer *r);

/* Editor: draw a full-bright wireframe box in world space (hover/selection highlight) */
void renderer_draw_wire_box(Renderer *r, Vec3f bmin, Vec3f bmax,
                            float cr, float cg, float cb);

/* Math helpers exposed for main.c */
void mat4_perspective(float *m, float fovy, float aspect, float near, float far);
void mat4_mul(float *out, const float *a, const float *b);
void mat4_identity(float *m);
