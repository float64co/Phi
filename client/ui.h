#pragma once
#include "font.h"
#include "svg_icon.h"
#include "octree_render.h"
#include "meshobject.h"
#include "console.h"
#include "asset_browser.h"
#include "chat.h"
#include "renderer.h"
#include "gbuffer.h"
#include "render_settings.h"
#include "input.h"
#include "skinned_mesh_object.h"

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
    PANEL_CURVE_EDITOR,  /* stub -- Phase 4's OTHER animation panel (bezier curve editing), still not started; see PANEL_TIMELINE below for the real one that IS */
    PANEL_ASSET_BROWSER, /* see asset_browser.h / phi.md's "Asset tracking and the Asset Browser panel" */
    PANEL_PYTHON,        /* @phi.panel-registered content, see mp_port.h / phi.md's "Python-extensible panels" */
    /* Phase 4's real scrub/play/pause panel (see phi.md's "Animation
     * Editor") -- operates on UIRenderContext::skinned_obj (main.c's one
     * skinned-test-object slot, see skinned_mesh_object.h), NOT the
     * bezier-curve-editing PANEL_CURVE_EDITOR above (a genuinely separate,
     * still-unstarted piece of Phase 4's UI). */
    PANEL_TIMELINE,
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
    /* Voronoi pre-fracture (Phase 1, see fracture.h) -- acts on whichever
     * MeshObject is selected as a whole, not a specific face, unlike the
     * three rows above. Fixed fragment count/seed/output path for this
     * pass (no interactive count-picker UI), see main.c's handler. */
    CTX_ACTION_FRACTURE,
    /* Asset Browser Create flow (see phi.md's "Wire protocol..." and
     * asset_browser.h) -- flattens the selected MeshObject and starts a
     * pending create (asset_browser_begin_create), NOT an immediate
     * upload; the actual POST only happens once the user fills in a name/
     * tags and submits from the Asset Browser panel's edit form. */
    CTX_ACTION_SAVE_AS_ASSET,
    /* Phase 2 (see phi.md's "Bullet Physics via Emscripten") -- creates a
     * dynamic rigid body for the selected MeshObject (box shape from its
     * own AABB, see meshobject_local_aabb_half_extents), so it starts
     * falling/colliding from then on. No-op (reported, not silently
     * ignored) if it already has one. */
    CTX_ACTION_ENABLE_PHYSICS,
    /* Blender-style Object/Edit mode toggle -- Object mode row reads "Enter
     * Edit Mode" (only shown when a MeshObject is selected), Edit mode row
     * reads "Exit Edit Mode" (always shown while in Edit mode). Mirrors the
     * Tab-key shortcut (see main.c's g_editor_mode) rather than duplicating
     * its logic -- both paths funnel through the same toggle_editor_mode()
     * helper in main.c. */
    CTX_ACTION_TOGGLE_EDIT_MODE,
    /* Spawns a new Light object at the Scene panel's current pivot (Phase
     * 3's "Both like Blender does it" light-source model, see light.h) --
     * always available regardless of what's currently selected, matching
     * Blender's own Add menu. One row per type rather than a real nested
     * submenu (this menu has no submenu mechanism, see ui.c's
     * build_ctx_menu_rows -- "Add > Mesh Object" is the same flat-row-with-
     * a-">"-in-the-label convention, not real nesting either). */
    CTX_ACTION_ADD_LIGHT_POINT,
    CTX_ACTION_ADD_LIGHT_SUN,
    CTX_ACTION_ADD_LIGHT_SPOT,
    CTX_ACTION_ADD_LIGHT_AREA,
} CtxMenuAction;

/* Blender-style Object/Edit mode -- see main.c's g_editor_mode (owns the
 * actual state; ui.c only reads it via UIRenderContext::editor_mode to
 * decide which context-menu rows to show/hit-test). Edit mode restricts
 * picking to face-selection within the already-selected MeshObject (see
 * main.c's try_pick_object); Object mode is everything this engine already
 * did before this enum existed (gizmo-handle drag, whole-object select). */
typedef enum {
    EDITOR_MODE_OBJECT = 0,
    EDITOR_MODE_EDIT,
} EditorMode;

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
    MeshObject *test_obj;        /* Phase 1's assets/cube.gltf test object, see main.c */
    int         test_obj_loaded;
    RenderSettings *render_settings;  /* main.c's g_render_settings (Phase 3, see render_settings.h) -- always non-NULL once main.c wires it up, shown as a pinned Properties-panel section regardless of selection. */
    /* main.c's one skinned-test-object slot (Phase 4, see skinned_mesh_
     * object.h) -- NULL until main.c has successfully loaded one (same
     * "NULL means nothing to show yet" convention test_obj_loaded's own
     * flag uses above, just via a pointer instead of a separate bool
     * since this slot's existence and its loadedness are the same
     * question). Read-only from ui.c's side -- the Timeline panel only
     * ever mutates it indirectly, through ui_poll_timeline_scrub/_play_
     * toggle below, which main.c drains and applies itself, same "UI
     * raises intent, main.c executes" split every other panel action in
     * this codebase already uses (context menu, top menu, Asset Browser
     * flags, ...). */
    SkinnedMeshObject *skinned_obj;
    int         edit_face;       /* main.c's g_edit_face -- last ray-picked hem face, -1 if none. Properties reads this for the per-face material readout. */
    EditorMode  editor_mode;     /* main.c's g_editor_mode -- see EditorMode's own comment. Read by the Scene panel's mode label and the right-click context menu's row set. */
    /* Modal G/S/R transform tool's HUD readout (see transform_op.h's
     * transform_op_hud_text) -- NULL or empty when no operation is
     * active. main.c formats the string, ui.c just draws it (same split
     * as editor_mode's own label above), next to the Object/Edit Mode
     * label in the Scene panel. */
    const char *xform_hud;
    PyConsoleState *console;
    AssetBrowserState *asset_browser;
    ChatState *chat;
    float       light_dir[3];
    float       sky_color[3];
    /* Called by the Scene panel between gbuffer_begin_geometry_pass() and
     * gbuffer_render_shadow_map() — issues every renderer_draw_* call for
     * the frame's game content (mesh objects, the transform gizmo). Kept
     * as a callback rather than ui.c calling those directly, so this UI
     * system doesn't need to know about scene-content specifics at all —
     * main.c owns exactly this sequence. Used to also draw Qek's world
     * mesh/players/rockets and the octree carve-editor overlay; those are
     * gone along with the rest of that code (see phi.md's Phase 1 status,
     * "Client/server model"), so this callback currently only draws
     * MeshObjects + gizmo, not "nothing else ever will." */
    void (*draw_scene_content)(void *userdata);
    void *draw_scene_userdata;
    /* Mouse-wheel-over-the-Scene-panel zoom (see ui_on_mouse_wheel) --
     * main.c owns the actual camera position/yaw/pitch (no real camera-
     * navigation system exists yet beyond this, see phi.md's Phase 1
     * status), so this is a callback rather than ui.c reaching into
     * renderer.c's camera fields directly, same division of labor
     * draw_scene_content already established. delta is InputState::
     * scroll_delta's own sign convention (positive = wheel scrolled up/
     * toward the user); main.c's implementation decides what that means
     * in world units (dolly-forward-by-N along the view direction). */
    void (*on_scene_zoom)(int delta);
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

/* Mouse wheel -- see ui.c's own comment on why the Chat panel's
 * scrollback is currently the only consumer. ctx is read for ctx->chat
 * only (the same pointer already passed to ui_render/ui_on_mouse_button). */
void ui_on_mouse_wheel(int x, int y, int delta, const UIRenderContext *ctx);

/* Blender-style area border drag-to-resize -- call once per frame (same
 * shape as the gizmo drag block in main.c) with the current mouse
 * position and whether LMB is held. No-op if nothing's being resized.
 * ui_is_resizing_area() lets main.c skip its own scene-click/gizmo-drag
 * handling for a frame where a border drag is in progress (a resize
 * shouldn't also register as a scene click underneath it). */
void ui_update_area_drag(int mouse_x, int mouse_y, int lmb_down);
int  ui_is_resizing_area(void);

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

/* Top menu bar (File/Edit/View/Help, see ui.c's draw_menu_row) -- only
 * File's two rows actually do anything main.c needs to act on (Save/Load
 * the selected mesh); Edit/View are placeholder dropdowns for now and
 * Help's rows (Keyboard Shortcuts/About) open a modal entirely within
 * ui.c, no main.c-side action needed. Same one-shot "drain it once per
 * frame" convention as ui_poll_context_menu_action above. */
typedef enum {
    TOP_ACTION_NONE = 0,
    TOP_ACTION_FILE_SAVE,   /* same "begin the Asset Browser's create flow for the selected MeshObject" as the Scene context menu's existing Save as Asset row */
    TOP_ACTION_FILE_LOAD,   /* same as the Asset Browser panel's own per-row Load button, for whichever asset is currently selected there */
    /* Phase 3's real offline path tracer (see path_tracer.h/phi.md) --
     * renders one still frame of whatever's currently in the scene from
     * the Scene panel's own current camera, using render_settings.h's
     * samples count, and writes a real PNG to disk. Synchronous (this
     * pass has no background-thread/progress-bar UI, a real, honestly
     * flagged scope limit -- see main.c's handler), so the editor will
     * visibly hang for the render's duration; fine for this pass's
     * single-still-frame scope, not fine for anything longer. */
    TOP_ACTION_FILE_RENDER,
} TopMenuAction;
TopMenuAction ui_poll_top_menu_action(void);

/* Phase 4's Timeline panel (PANEL_TIMELINE) -- scrub bar + Play/Pause
 * button, same one-shot "drain it once per frame" convention as
 * ui_poll_top_menu_action/ui_poll_context_menu_action above (main.c owns
 * skinned_obj->playback, ui.c only ever raises intent, never mutates it
 * directly -- see UIRenderContext::skinned_obj's own comment).
 *
 * ui_poll_timeline_scrub: returns 1 and sets *out_fraction ([0,1], where
 * the click landed along the scrub bar) if the scrub bar was clicked/
 * dragged this frame, 0 otherwise (leaving *out_fraction untouched). */
int ui_poll_timeline_scrub(float *out_fraction);

/* Returns 1 once (drained, same one-shot convention) if the Play/Pause
 * button was clicked this frame, 0 otherwise -- main.c flips its own
 * playback.playing in response, this doesn't carry the new state itself
 * (ui.c doesn't own that state to know which direction "toggle" even
 * means without asking main.c first). */
int ui_poll_timeline_play_toggle(void);

/* object_id follows the existing scheme (renderer.h/gbuffer.c): wire-box=3,
 * MeshObjects=4000+id, Lights=5000+id (see main.c's selected_light()).
 * 0xFFFFFFFF = nothing selected. (world=0/ground=1/players=1000+id/
 * rockets=2000+slot belonged to Qek's now-removed world/gameplay draw
 * calls -- those ranges are simply unused now.) */
void         ui_set_selected_object(unsigned int object_id);
unsigned int ui_get_selected_object(void);

/* Current on-screen rect of the Scene panel (top-left origin, y-down), for
 * main.c to know whether/where a click should be treated as "inside the
 * 3D viewport" — the Scene panel no longer always fills the whole window.
 * Returns 0 (fields untouched) if no Scene panel exists in the layout
 * right now (the type-switcher lets a user swap it away). */
int ui_get_scene_rect(float *x, float *y, float *w, float *h);

/* Properties-panel inline field editing (see UIRenderContext-adjacent
 * PropHitResult/properties_row in ui.c) -- same "no-op when nothing of
 * this panel's own is focused, so falling through to the console is
 * always correct" contract asset_browser_update_focused_text/
 * chat_update_focused_text already establish. Call from main.c's own
 * focus-priority chain, same level as those two. Drains typed_chars
 * (digits/'.'/'-'/','/space only -- anything else silently ignored
 * rather than accepted then failing to parse) and backspace_edge always
 * when a field is focused; Enter commits the typed value through
 * phi_prop_set_float/vec3 and clears focus. */
void ui_update_prop_edit_text(InputState *inp);

/* True while a Properties-panel field is being edited -- main.c checks
 * this to route typed_chars/backspace_edge/enter_edge to
 * ui_update_prop_edit_text instead of falling through to the console,
 * same "which one thing owns keyboard input right now" priority chain
 * asset_browser/chat focus already slot into. */
int ui_is_editing_prop(void);

/* True while a modal (Keyboard Shortcuts/About) is open -- main.c checks
 * this to suppress Scene-panel picking and the G/S/R modal transform
 * tool's own entry, same reasoning ui_is_editing_prop already gates
 * those on: a modal owns the whole screen (it dims everything behind it
 * and any click just closes it, see ui.c's draw_modal_box), so nothing
 * underneath should react to input while one is up. */
int ui_is_modal_open(void);
