#pragma once
#include <stdint.h>

#define TYPED_CHAR_QUEUE_SIZE 32

/* Mouse/keyboard input state -- previously also carried Qek's FPS
 * movement/look/combat fields (forward/back/left/right/jump/fire/yaw/
 * pitch/sensitivity/pointer_locked/input_seq) and its octree-editor
 * fields (edit_toggle/up/down/paint_mod/shift/crouch/grid_inc/grid_dec/
 * mat_inc/mat_dec/export_stl), all removed along with the code that
 * consumed them (see
 * phi.md's Phase 1 status, "Client/server model"). What remains is
 * exactly what the Native UI System and the Python panel actually use:
 * real cursor position/clicks for panel hit-testing and gizmo dragging,
 * and text entry for the panel. */
typedef struct {
    int   lmb_down, rmb_down;  /* raw mouse button state */
    /* Middle mouse button -- added for Blender-style Scene-panel camera
     * navigation (orbit on plain MMB-drag, pan on Ctrl+MMB-drag, see
     * main.c's scene_camera_drag handling). Same raw-held-state shape as
     * lmb_down/rmb_down, not a click edge, since a drag needs to know
     * "still held" every frame, not just the press moment. */
    int   mmb_down;
    /* Ctrl key, held state -- only other modifier this codebase tracks
     * (no Shift/Alt yet), needed to distinguish MMB-orbit from MMB-pan at
     * the moment a drag STARTS (matching Blender's own "modifier state at
     * click time, not live-toggled mid-drag" behavior). */
    int   ctrl_down;

    /* Absolute window-local cursor position (y-down), tracked
     * unconditionally on every mouse-move -- UI panel hit-testing
     * (ui_on_mouse_button) and gizmo dragging need a real cursor position
     * every frame, not just on click edges. */
    int   mouse_x, mouse_y;
    /* Rising edge: left/right/middle mouse button pressed this frame, at
     * (mouse_x, mouse_y) when read -- main.c drains and clears these once
     * per frame to route real clicks into ui_on_mouse_button()/scene
     * picking, or (mmb_click) to start a camera orbit/pan drag. Distinct
     * from lmb_down/rmb_down/mmb_down (held state). */
    int   lmb_click, rmb_click, mmb_click;

    /* Python-panel text entry — the panel acts as an always-focused text
     * input (see console.c): input.c captures these unconditionally each
     * frame and console.c consumes them every frame. There is no
     * open/close focus toggle. */
    char  typed_chars[TYPED_CHAR_QUEUE_SIZE];
    int   typed_count;
    int   enter_edge, backspace_edge;
    int   histup_edge, histdown_edge;   /* Up / Down arrows: input history */
    /* Tab key, rising edge -- reserved globally for the Object/Edit mode
     * toggle (see main.c's g_editor_mode), same "reserved function key"
     * carve-out console.c's own comment already describes for Enter/
     * Backspace/arrows: never queued into typed_chars (all three platforms
     * already exclude it there the same way they exclude Enter), consumed
     * once per frame by main.c rather than by the always-focused Python
     * panel. */
    int   tab_edge;
    /* Escape key, rising edge -- reserved for the modal G/S/R transform
     * tool's cancel action (see transform_op.h), NOT a resurrection of the
     * old console open/close modality's escape_edge (that field was
     * deliberately removed along with backquote-toggled console focus,
     * see phi.md's "Python panel is now an always-focused text input").
     * Same "never queued into typed_chars" exclusion as Tab/Enter/
     * Backspace on all three platforms. */
    int   escape_edge;

    /* Mouse wheel, accumulated (there can be more than one wheel event per
     * frame) since the last time a consumer drained it to 0 -- same
     * producer-sets/consumer-clears convention as lmb_click. Positive =
     * scrolled up/toward the user (reveals OLDER content, e.g. further
     * back in the Chat panel's scrollback); negative = scrolled down.
     * Previously captured by all three platforms' event handlers but never
     * wired to anything (see input.c's own history here) -- now consumed
     * by ui_on_mouse_wheel for the Chat panel's scrollback. */
    int   scroll_delta;
} InputState;

void input_init(InputState *inp);
void input_install_callbacks(InputState *inp);  /* registers JS event listeners */

#ifndef __EMSCRIPTEN__
/* Called by phi_platform_native.c's event pump for every XEvent it doesn't
 * itself handle (resize/close). Takes `void *` rather than `XEvent *` so
 * this header doesn't need to pull in <X11/Xlib.h> for callers that only
 * ever see the wasm build. No-op today — real key/mouse translation is a
 * separate, not-yet-landed piece of native input support. */
void input_native_handle_event(void *xevent);
#endif
