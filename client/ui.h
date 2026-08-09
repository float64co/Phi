#pragma once
#include "font.h"
#include "svg_icon.h"
#include "octree_render.h"
#include "meshobject.h"
#include "physics.h"
#include "console.h"
#include "renderer.h"
#include "gbuffer.h"

/* Native UI System — Phase 1 (see phi.md's "Native UI System" section for
 * the design this implements: Blender's recursive area-split model, not
 * Zenith's simpler fixed-slot dock, deliberately). Built in C, rendered as
 * a 2D overlay via WebGL calls on top of the already-resolved 3D frame —
 * same "not through the panel system" split phi.md already specified for
 * gizmos/overlays vs. panel chrome, just applied to this being the panel
 * chrome itself.
 *
 * Golden ratio throughout, matching the reasoning a separate reference
 * project used for its own UI constants (read this session, not copied —
 * that project is CPython+pybind11, this is plain C): UI_PHI is the actual
 * golden ratio, used both for the default panel split proportions and for
 * consistent chrome sizing (padding, bar height) the same way FONT_SIZE/
 * WIDGET_H/PADDING related to each other there.
 */

#define UI_PHI       1.6180339887f
#define UI_INV_PHI   0.6180339887f   /* 1/PHI == PHI-1, the "larger" golden fraction */

/* Chrome sizing derived from a single base unit via PHI, the same relationship
 * a separate reference project used for its own UI constants (FONT_SIZE ->
 * WIDGET_H -> HEADER_H, each the last times PHI or SQRT_PHI) — golden ratio
 * "with respect to the whole layout" means chrome heights are related to
 * each other and to the font size by the same ratio the panel splits use,
 * not an arbitrary flat pixel count. UI_MENU_H (the menu row) is the base
 * font size stepped up once by PHI; UI_BAR_H (the branding row above it) is
 * stepped up once more — shorter than the original flat 48px branding bar,
 * per the explicit request to reduce it. */
#define UI_FONT_SIZE  14.0f
#define UI_MENU_H     (UI_FONT_SIZE * UI_PHI)         /* ~22.65 */
#define UI_BAR_H      (UI_MENU_H   * UI_PHI)          /* ~36.65 */
#define UI_TOP_CHROME_H (UI_BAR_H + UI_MENU_H)        /* branding row + menu row together */
#define UI_PANEL_PAD  10.0f
#define UI_TYPE_ICON_SIZE 14.0f  /* per-panel corner type-switcher button -- top-left corner, see draw_area_chrome */

typedef enum {
    PANEL_SCENE = 0,
    PANEL_OUTLINER,
    PANEL_PROPERTIES,
    PANEL_CONSOLE,
    PANEL_CHAT,
    PANEL_NODE_EDITOR,   /* stub -- Phase 6 not started */
    PANEL_CURVE_EDITOR,  /* stub -- Phase 4 not started */
    PANEL_TYPE_COUNT
} PanelType;

typedef enum { AREA_LEAF, AREA_SPLIT_H, AREA_SPLIT_V } AreaKind;

/* Scene right-click context menu rows -- see ui_poll_context_menu_action().
 * Order matches the menu's own items[] array in ui.c. */
typedef enum {
    CTX_ACTION_NONE = 0,
    CTX_ACTION_ADD_MESH,
    CTX_ACTION_DELETE,
    CTX_ACTION_FRAME_SELECTED,
    CTX_ACTION_FRAME_ALL,
    CTX_ACTION_DESELECT_ALL,
    /* Mesh-editing ops (Phase 1, see mesh_edit.h) -- act on whichever face
     * main.c's rmb_click handler last ray-picked on the selected
     * MeshObject at the moment the menu was opened (see main.c's
     * g_edit_face). Menu rows always show; main.c's switch on these is
     * what reports "no face under the click" rather than the menu itself
     * conditionally hiding them. */
    CTX_ACTION_EXTRUDE_FACE,
    CTX_ACTION_INSET_FACE,
    CTX_ACTION_LOOP_CUT,
} CtxMenuAction;

typedef struct Area {
    AreaKind kind;
    float split;              /* SPLIT_H/V only: 0..1 fraction going to child[0] */
    struct Area *child[2];    /* SPLIT_H: [0]=left,[1]=right. SPLIT_V: [0]=top,[1]=bottom */
    PanelType panel_type;     /* LEAF only */
    /* Computed fresh each frame by ui_layout(), top-left origin, y-down
     * (screen/mouse convention) — NOT GL's bottom-left convention; that
     * conversion happens once, at the point something actually issues a
     * glViewport call (see ui_draw_scene_panel's gbuffer_set_viewport_offset
     * call in ui.c). */
    float x, y, w, h;
    /* Per-area type-switcher dropdown state (see UI_TYPE_ICON_SIZE) */
    int   type_menu_open;
} Area;

/* Loads fonts + icons, builds the default golden-ratio-proportioned area
 * tree (Scene left ~61.8% width; right column splits ~61.8%/38.2% into
 * Outliner top / Properties bottom). Must run after the GL context exists
 * (same requirement as renderer_create/gbuffer_create). Returns 0 on
 * failure (a required font/icon failed to load). */
int  ui_init(void);
void ui_destroy(void);

/* Recomputes every Area's x/y/w/h from the current window size — call
 * once per frame before ui_render(), after any resize. */
void ui_layout(int window_w, int window_h);

/* Everything ui_render() needs to draw real panel content — explicit
 * fields rather than reaching for main.c's globals, matching how the rest
 * of this codebase passes context (e.g. renderer_draw_players(Renderer*,
 * const GameState*, int)) rather than hiding cross-file coupling behind
 * extern globals. */
typedef struct {
    Renderer   *renderer;
    GBuffer    *gbuf;
    RenderMesh *world_mesh;
    GameState  *gs;
    int         local_player_id;
    MeshObject *test_obj;        /* Phase 1's assets/cube.gltf test object, see main.c */
    int         test_obj_loaded;
    int         edit_face;       /* main.c's g_edit_face -- last ray-picked hem face, -1 if none. Properties reads this for the per-face material readout. */
    ConsoleState *console;
    float       light_dir[3];
    float       sky_color[3];
    /* Called by the Scene panel between gbuffer_begin_geometry_pass() and
     * gbuffer_render_shadow_map() — issues every renderer_draw_* call for
     * the frame's game content (world/ground/players/rockets/mesh objects/
     * editor overlay). Kept as a callback rather than ui.c calling those
     * directly, so this UI system doesn't need to know about Qek-specific
     * entities (players/rockets) at all — main.c already owns exactly
     * this sequence for its own (pre-panel-system) render loop. */
    void (*draw_scene_content)(void *userdata);
    void *draw_scene_userdata;
} UIRenderContext;

/* Draws the branding bar + every panel's chrome and content. Panels whose
 * content needs 3D rendering (currently only Scene) also drive the
 * G-buffer pipeline for their own sub-rectangle from here — see ui.c. */
void ui_render(const UIRenderContext *ctx);

/* Input routing (screen coords, y-down, same convention input.c already
 * uses) — returns 1 if the UI consumed the event (so callers like the 3D
 * viewport's camera-look/fire input should NOT also act on it), 0 if the
 * event passed through (e.g. a click inside the Scene panel's content,
 * away from its own chrome). ctx is only read for object ids it already
 * knows about (e.g. Outliner-row selection) — safe to pass the same
 * pointer used for ui_render(). */
int  ui_on_mouse_button(int x, int y, int button, int pressed, const UIRenderContext *ctx);
void ui_on_mouse_move(int x, int y);

/* Opens the 3D scene's right-click context menu at (x,y) — called from
 * main.c when a right-click lands inside the Scene panel's content area,
 * not its chrome. See ui.c's context-menu section. */
void ui_open_scene_context_menu(int x, int y);
int  ui_is_context_menu_open(void);

/* Drains and clears whichever scene-context-menu row was last clicked (if
 * any) -- same one-shot "drain it once per frame" convention as
 * InputState's lmb_click etc. (see input.h).
 * CTX_ACTION_ADD_MESH/DELETE/EXTRUDE_FACE/INSET_FACE/LOOP_CUT are wired to
 * real behavior in main.c; Frame Selected/Frame All/Deselect All rows
 * still just report which one was clicked. */
CtxMenuAction ui_poll_context_menu_action(void);

/* object_id follows the existing scheme (renderer.h/gbuffer.c): world=0,
 * ground=1, players=1000+id, rockets=2000+slot, wire-box=3,
 * MeshObjects=4000+id. 0xFFFFFFFF = nothing selected. */
void         ui_set_selected_object(unsigned int object_id);
unsigned int ui_get_selected_object(void);

/* Current on-screen rect of the Scene panel (top-left origin, y-down), for
 * main.c to know whether/where a click should be treated as "inside the
 * 3D viewport" — the Scene panel no longer always fills the whole window.
 * Returns 0 (fields untouched) if no Scene panel exists in the layout
 * right now (the type-switcher lets a user swap it away). */
int ui_get_scene_rect(float *x, float *y, float *w, float *h);
