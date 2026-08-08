#pragma once
#include "octree.h"
#include "physics.h"
#include "input.h"
#include "net.h"
#include "renderer.h"

/* Sauerbraten-style octree editor: raycast + grid-snapped click-drag box
 * selection. LMB drag builds solid outward from the hovered face, RMB drag
 * carves empty inward, M+LMB drag repaints material without touching
 * geometry. Every drag is exactly 1 grid unit deep — repeat drags to build
 * up thicker structures (see peppy-weaving-forest.md for the full spec). */
typedef enum {
    ED_IDLE = 0,
    ED_DRAG_SOLID,
    ED_DRAG_EMPTY,
    ED_DRAG_MATERIAL,
} EditorDragMode;

typedef struct {
    int active;         /* editor mode on/off (E) */
    int grid_pow;        /* grid size = 1 << grid_pow, clamped to [1,9] -> 2..512 */
    int cur_mat;         /* current paint material id, 0..7 */

    /* Hover target — recomputed every frame while idle (not dragging) */
    int has_target;
    int target_min[3], target_max[3];   /* grid-aligned box of the hovered solid cube */
    int target_face;                     /* FACE_* the ray hit */

    /* In-progress drag */
    EditorDragMode drag_mode;
    int   drag_min[3], drag_max[3];      /* grows as the user drags */
    int   drag_anchor[3];
    int   drag_face;
    int   drag_plane_axis;
    float drag_plane_coord;

    int prev_lmb, prev_rmb;              /* for edge detection on the raw held-state input flags */
    int world_dirty;                     /* set on commit; main.c rebuilds/uploads the mesh, then clears it */

    char status[128];                    /* HUD line, rebuilt each frame while active */
} EditorState;

void editor_init(EditorState *ed);
void editor_update(EditorState *ed, GameState *gs, Player *local,
                    InputState *inp, NetState *ns, float dt);
void editor_render(const EditorState *ed, Renderer *r);
