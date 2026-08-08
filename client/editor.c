#include "editor.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define EDITOR_FLY_SPEED        320.0f
#define EDITOR_FLY_SPRINT_MULT  3.0f
#define EDITOR_MAX_RAY          1024.0f

void editor_init(EditorState *ed) {
    memset(ed, 0, sizeof(*ed));
    ed->grid_pow = 3;  /* default grid = 8 */
    ed->cur_mat  = 0;
}

static Vec3f view_dir(float yaw, float pitch) {
    float sy = sinf(yaw),  cy = cosf(yaw);
    float sp = sinf(pitch), cp = cosf(pitch);
    Vec3f d = { -sy*cp, sp, -cy*cp };
    return d;
}

static void fly_move(Player *p, const InputState *inp, float dt) {
    Vec3f fwd = view_dir(p->yaw, p->pitch);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    Vec3f right = { cy, 0.0f, -sy };
    Vec3f wish = {0,0,0};
    if (inp->forward) { wish.x += fwd.x;   wish.y += fwd.y;   wish.z += fwd.z; }
    if (inp->back)    { wish.x -= fwd.x;   wish.y -= fwd.y;   wish.z -= fwd.z; }
    if (inp->left)    { wish.x -= right.x; wish.z -= right.z; }
    if (inp->right)   { wish.x += right.x; wish.z += right.z; }
    if (inp->up)        wish.y += 1.0f;
    if (inp->down)      wish.y -= 1.0f;

    float len = sqrtf(wish.x*wish.x + wish.y*wish.y + wish.z*wish.z);
    if (len > 1e-4f) {
        float speed = EDITOR_FLY_SPEED * (inp->shift ? EDITOR_FLY_SPRINT_MULT : 1.0f);
        p->pos.x += wish.x/len * speed * dt;
        p->pos.y += wish.y/len * speed * dt;
        p->pos.z += wish.z/len * speed * dt;
    }
    p->vel = (Vec3f){0,0,0};
    p->on_ground = 0;
}

static void face_axis_sign(int face, int *axis, int *sign) {
    switch (face) {
        case FACE_NEG_X: *axis = 0; *sign = -1; break;
        case FACE_POS_X: *axis = 0; *sign =  1; break;
        case FACE_NEG_Y: *axis = 1; *sign = -1; break;
        case FACE_POS_Y: *axis = 1; *sign =  1; break;
        case FACE_NEG_Z: *axis = 2; *sign = -1; break;
        default:         *axis = 2; *sign =  1; break; /* FACE_POS_Z */
    }
}

static void update_hover_target(EditorState *ed, const Octree *world, Vec3f eye, Vec3f dir) {
    Vec3f hit_pos, hit_normal = {0,0,0};
    float t = octree_ray_cast(world, eye, dir, EDITOR_MAX_RAY, &hit_pos, &hit_normal);
    if (t < 0) {
        /* Nothing solid along the ray — e.g. the whole map has been carved
         * away. physics.c still hard-clamps every player to y>=16
         * regardless of octree content (see physics_move_player), so fall
         * back to targeting that implicit floor plane whenever we're
         * looking down at it, otherwise there'd be no face left to build
         * outward from ever again. */
        if (dir.y < -1e-4f && eye.y > 16.0f) {
            float tp = (16.0f - eye.y) / dir.y;
            if (tp >= 0.0f && tp <= EDITOR_MAX_RAY) {
                float px = eye.x + dir.x * tp;
                float pz = eye.z + dir.z * tp;
                if (px >= 0.0f && px < (float)WORLD_SIZE &&
                    pz >= 0.0f && pz < (float)WORLD_SIZE) {
                    hit_pos    = (Vec3f){px, 16.0f, pz};
                    hit_normal = (Vec3f){0.0f, 1.0f, 0.0f};
                    t = tp;
                }
            }
        }
        if (t < 0) { ed->has_target = 0; return; }
    }

    int face;
    if      (hit_normal.x < -0.5f) face = FACE_NEG_X;
    else if (hit_normal.x >  0.5f) face = FACE_POS_X;
    else if (hit_normal.y < -0.5f) face = FACE_NEG_Y;
    else if (hit_normal.y >  0.5f) face = FACE_POS_Y;
    else if (hit_normal.z < -0.5f) face = FACE_NEG_Z;
    else                            face = FACE_POS_Z;

    int grid = 1 << ed->grid_pow;
    /* Nudge slightly into the solid along -normal so floor() lands inside
     * the hit cube rather than exactly on its boundary. */
    float ix = hit_pos.x - hit_normal.x * 0.5f;
    float iy = hit_pos.y - hit_normal.y * 0.5f;
    float iz = hit_pos.z - hit_normal.z * 0.5f;
    int mx = (int)floorf(ix / grid) * grid;
    int my = (int)floorf(iy / grid) * grid;
    int mz = (int)floorf(iz / grid) * grid;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mz < 0) mz = 0;
    if (mx > WORLD_SIZE - grid) mx = WORLD_SIZE - grid;
    if (my > WORLD_SIZE - grid) my = WORLD_SIZE - grid;
    if (mz > WORLD_SIZE - grid) mz = WORLD_SIZE - grid;

    ed->target_min[0] = mx; ed->target_min[1] = my; ed->target_min[2] = mz;
    ed->target_max[0] = mx+grid; ed->target_max[1] = my+grid; ed->target_max[2] = mz+grid;
    ed->target_face = face;
    ed->has_target  = 1;
}

static void start_drag(EditorState *ed, EditorDragMode mode) {
    int axis, sign;
    face_axis_sign(ed->target_face, &axis, &sign);
    int grid = 1 << ed->grid_pow;

    int bmin[3] = { ed->target_min[0], ed->target_min[1], ed->target_min[2] };
    int bmax[3] = { ed->target_max[0], ed->target_max[1], ed->target_max[2] };

    /* The tangent-sweep plane is the ORIGINAL hit face, before any shift
     * below, so dragging tracks the surface actually clicked on. */
    ed->drag_plane_axis  = axis;
    ed->drag_plane_coord = (float)((sign > 0) ? ed->target_max[axis] : ed->target_min[axis]);

    if (mode == ED_DRAG_SOLID) {
        /* Shift one grid unit outward (away from the solid) along the hit axis. */
        if (sign > 0) { bmin[axis] = bmax[axis]; bmax[axis] = bmin[axis] + grid; }
        else          { bmax[axis] = bmin[axis]; bmin[axis] = bmax[axis] - grid; }
    }
    /* EMPTY / MATERIAL: box stays exactly on the hit (solid) cube. */

    for (int i = 0; i < 3; i++) {
        if (bmin[i] < 0) bmin[i] = 0;
        if (bmax[i] > WORLD_SIZE) bmax[i] = WORLD_SIZE;
        ed->drag_min[i] = bmin[i];
        ed->drag_max[i] = bmax[i];
        ed->drag_anchor[i] = bmin[i];
    }
    ed->drag_mode = mode;
    ed->drag_face = ed->target_face;
}

/* Where the crosshair currently lands on the drag's tangent plane (the
 * plane through the depth axis fixed in start_drag), as world coords for
 * the two tangent axes. Tries an actual raycast against the world first —
 * so redirecting mid-drag "follows the mouse" onto whatever geometry
 * (any wall/corner, not just the original clicked face) is under the
 * crosshair now — and only falls back to intersecting the infinite plane
 * (which fails whenever the view has tilted away from it) when that ray
 * hits nothing. Returns 0 if neither yields a usable point (keep last box). */
static int drag_cursor_point(const Octree *world, int axis, float plane_coord,
                              Vec3f eye, Vec3f dir, float *out_tan0, float *out_tan1) {
    int tan0 = (axis + 1) % 3, tan1 = (axis + 2) % 3;

    Vec3f hp;
    float t = octree_ray_cast(world, eye, dir, EDITOR_MAX_RAY, &hp, NULL);
    if (t >= 0) {
        float hpa[3] = { hp.x, hp.y, hp.z };
        *out_tan0 = hpa[tan0];
        *out_tan1 = hpa[tan1];
        return 1;
    }

    float dirv[3] = { dir.x, dir.y, dir.z };
    float eyev[3] = { eye.x, eye.y, eye.z };
    if (fabsf(dirv[axis]) < 1e-5f) return 0;   /* looking parallel to the plane */
    float tp = (plane_coord - eyev[axis]) / dirv[axis];
    if (tp < 0) return 0;                       /* plane is behind the camera */
    float hit[3] = { eyev[0]+dirv[0]*tp, eyev[1]+dirv[1]*tp, eyev[2]+dirv[2]*tp };
    *out_tan0 = hit[tan0];
    *out_tan1 = hit[tan1];
    return 1;
}

static void update_drag_box(EditorState *ed, const Octree *world, Vec3f eye, Vec3f dir) {
    int axis = ed->drag_plane_axis;
    float tan0v, tan1v;
    if (!drag_cursor_point(world, axis, ed->drag_plane_coord, eye, dir, &tan0v, &tan1v))
        return;   /* nothing sensible to follow this frame — keep last box */

    int grid = 1 << ed->grid_pow;
    int tan0 = (axis + 1) % 3, tan1 = (axis + 2) % 3;
    int cell[3] = {0,0,0};
    cell[tan0] = (int)floorf(tan0v / grid) * grid;
    cell[tan1] = (int)floorf(tan1v / grid) * grid;

    int tangents[2] = { tan0, tan1 };
    for (int k = 0; k < 2; k++) {
        int a  = tangents[k];
        int lo = ed->drag_anchor[a] < cell[a] ? ed->drag_anchor[a] : cell[a];
        int hi = (ed->drag_anchor[a] > cell[a] ? ed->drag_anchor[a] : cell[a]) + grid;
        if (lo < 0) lo = 0;
        if (hi > WORLD_SIZE) hi = WORLD_SIZE;
        ed->drag_min[a] = lo;
        ed->drag_max[a] = hi;
    }
    /* depth axis (drag_face's axis) stays fixed as set in start_drag */
}

static void commit_drag(EditorState *ed, GameState *gs, NetState *ns) {
    int op = (ed->drag_mode == ED_DRAG_SOLID) ? EDIT_OP_SOLID
           : (ed->drag_mode == ED_DRAG_EMPTY) ? EDIT_OP_EMPTY
           : EDIT_OP_MATERIAL;

    int *mn = ed->drag_min, *mx = ed->drag_max;
    if (op == EDIT_OP_SOLID)
        octree_set_solid(gs->world, mn[0],mn[1],mn[2], mx[0],mx[1],mx[2]);
    else if (op == EDIT_OP_EMPTY)
        octree_set_empty(gs->world, mn[0],mn[1],mn[2], mx[0],mx[1],mx[2]);
    else
        octree_set_material(gs->world, mn[0],mn[1],mn[2], mx[0],mx[1],mx[2],
                             ed->drag_face, ed->cur_mat);

    net_send_edit_region(ns, op, ed->drag_face, ed->cur_mat,
                          mn[0],mn[1],mn[2], mx[0],mx[1],mx[2]);

    ed->world_dirty = 1;
    ed->drag_mode = ED_IDLE;
}

void editor_update(EditorState *ed, GameState *gs, Player *local,
                    InputState *inp, NetState *ns, float dt) {
    ed->world_dirty = 0;

    if (inp->edit_toggle) {
        inp->edit_toggle = 0;
        ed->active = !ed->active;
        if (!ed->active) { ed->drag_mode = ED_IDLE; ed->has_target = 0; }
    }

    /* physics_update() (physics.c) skips gravity/collision entirely for
     * noclip players — keeps fly_move() below authoritative over pos/vel. */
    local->noclip = ed->active;

    if (!ed->active) {
        ed->prev_lmb = inp->lmb_down;
        ed->prev_rmb = inp->rmb_down;
        return;
    }

    if (inp->grid_inc) { inp->grid_inc = 0; if (ed->grid_pow < 9) ed->grid_pow++; }
    if (inp->grid_dec) { inp->grid_dec = 0; if (ed->grid_pow > 1) ed->grid_pow--; }
    if (inp->mat_inc)  { inp->mat_inc  = 0; ed->cur_mat = (ed->cur_mat + 1) % 8; }
    if (inp->mat_dec)  { inp->mat_dec  = 0; ed->cur_mat = (ed->cur_mat + 7) % 8; }

    /* main.c skips physics_apply_input() while editing (fly_move owns
     * movement instead), but that's normally what copies mouse-look from
     * InputState into the player — so mirror just the yaw/pitch part here. */
    local->yaw   = inp->yaw;
    local->pitch = inp->pitch;

    fly_move(local, inp, dt);

    Vec3f eye = { local->pos.x, local->pos.y + PLAYER_EYE_H, local->pos.z };
    Vec3f dir = view_dir(local->yaw, local->pitch);

    if (ed->drag_mode == ED_IDLE) {
        update_hover_target(ed, gs->world, eye, dir);

        int lmb_edge = inp->lmb_down && !ed->prev_lmb;
        int rmb_edge = inp->rmb_down && !ed->prev_rmb;
        if (ed->has_target && lmb_edge)
            start_drag(ed, inp->paint_mod ? ED_DRAG_MATERIAL : ED_DRAG_SOLID);
        else if (ed->has_target && rmb_edge)
            start_drag(ed, ED_DRAG_EMPTY);
    } else {
        update_drag_box(ed, gs->world, eye, dir);

        int release_lmb = !inp->lmb_down && ed->prev_lmb &&
                           (ed->drag_mode == ED_DRAG_SOLID || ed->drag_mode == ED_DRAG_MATERIAL);
        int release_rmb = !inp->rmb_down && ed->prev_rmb && ed->drag_mode == ED_DRAG_EMPTY;
        if (release_lmb || release_rmb) commit_drag(ed, gs, ns);
    }

    ed->prev_lmb = inp->lmb_down;
    ed->prev_rmb = inp->rmb_down;

    const char *mode_str =
        ed->drag_mode == ED_DRAG_SOLID    ? "building..." :
        ed->drag_mode == ED_DRAG_EMPTY    ? "carving..."  :
        ed->drag_mode == ED_DRAG_MATERIAL ? "painting..." :
        ed->has_target ? "LMB build | RMB carve | M+LMB paint" : "(no target)";
    snprintf(ed->status, sizeof(ed->status), "EDIT  grid=%d  mat=%d  %s",
              1 << ed->grid_pow, ed->cur_mat, mode_str);
}

void editor_render(const EditorState *ed, Renderer *r) {
    if (!ed->active) return;

    if (ed->drag_mode == ED_IDLE) {
        if (!ed->has_target) return;
        Vec3f mn = { (float)ed->target_min[0], (float)ed->target_min[1], (float)ed->target_min[2] };
        Vec3f mx = { (float)ed->target_max[0], (float)ed->target_max[1], (float)ed->target_max[2] };
        renderer_draw_wire_box(r, mn, mx, 1.0f, 1.0f, 0.2f);
        return;
    }

    Vec3f mn = { (float)ed->drag_min[0], (float)ed->drag_min[1], (float)ed->drag_min[2] };
    Vec3f mx = { (float)ed->drag_max[0], (float)ed->drag_max[1], (float)ed->drag_max[2] };
    float cr, cg, cb;
    if      (ed->drag_mode == ED_DRAG_SOLID)    { cr=0.2f; cg=1.0f; cb=0.3f; }
    else if (ed->drag_mode == ED_DRAG_EMPTY)    { cr=1.0f; cg=0.3f; cb=0.2f; }
    else /* ED_DRAG_MATERIAL */                 { cr=0.2f; cg=0.8f; cb=1.0f; }
    renderer_draw_wire_box(r, mn, mx, cr, cg, cb);
}
