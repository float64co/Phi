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

    /* Absolute window-local cursor position (y-down), tracked
     * unconditionally on every mouse-move -- UI panel hit-testing
     * (ui_on_mouse_button) and gizmo dragging need a real cursor position
     * every frame, not just on click edges. */
    int   mouse_x, mouse_y;
    /* Rising edge: left/right mouse button pressed this frame, at
     * (mouse_x, mouse_y) when read -- main.c drains and clears these once
     * per frame to route real clicks into ui_on_mouse_button()/scene
     * picking. Distinct from lmb_down/rmb_down (held state). */
    int   lmb_click, rmb_click;

    /* Python-panel text entry — the panel acts as an always-focused text
     * input (see console.c): input.c captures these unconditionally each
     * frame and console.c consumes them every frame. There is no
     * open/close focus toggle. */
    char  typed_chars[TYPED_CHAR_QUEUE_SIZE];
    int   typed_count;
    int   enter_edge, backspace_edge;
    int   histup_edge, histdown_edge;   /* Up / Down arrows: input history */
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
