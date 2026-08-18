#pragma once
#include <stdint.h>

#define TYPED_CHAR_QUEUE_SIZE 32

/* Portable held-key state (Phase 9 gap-closing, 2026-08-18 — see phi.md's
 * Phase 9 "Known gaps": the player build had no general keyboard input at
 * all, only the text-entry-oriented edges below). A-Z, 0-9, and the
 * common gameplay/menu keys — NOT a full keyboard (no F-keys, no
 * punctuation), a real, stated MVP scope matching PhiGamepadButton's own
 * "classic stable set, not everything" precedent in input_gamepad.h.
 * Portable across all three backends: each one's own raw keycode/keysym/
 * JS-code is translated to this enum at the point where it already
 * handles key events, so nothing outside input.c ever sees a
 * platform-specific key code. */
typedef enum {
    PHI_KEY_A, PHI_KEY_B, PHI_KEY_C, PHI_KEY_D, PHI_KEY_E, PHI_KEY_F,
    PHI_KEY_G, PHI_KEY_H, PHI_KEY_I, PHI_KEY_J, PHI_KEY_K, PHI_KEY_L,
    PHI_KEY_M, PHI_KEY_N, PHI_KEY_O, PHI_KEY_P, PHI_KEY_Q, PHI_KEY_R,
    PHI_KEY_S, PHI_KEY_T, PHI_KEY_U, PHI_KEY_V, PHI_KEY_W, PHI_KEY_X,
    PHI_KEY_Y, PHI_KEY_Z,
    PHI_KEY_0, PHI_KEY_1, PHI_KEY_2, PHI_KEY_3, PHI_KEY_4,
    PHI_KEY_5, PHI_KEY_6, PHI_KEY_7, PHI_KEY_8, PHI_KEY_9,
    PHI_KEY_SPACE, PHI_KEY_SHIFT, PHI_KEY_CTRL,
    PHI_KEY_UP, PHI_KEY_DOWN, PHI_KEY_LEFT, PHI_KEY_RIGHT,
    PHI_KEY_ENTER, PHI_KEY_ESCAPE, PHI_KEY_TAB,
    PHI_KEY_COUNT,
} PhiKey;

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

    /* Held-state, 1/0, indexed by PhiKey above -- NOT cleared on read (a
     * game polls "is W held right now" every frame, unlike the one-shot
     * *_edge fields above which the UI drains). Real state, not a stub:
     * every backend below sets/clears these on the platform's own real
     * key-down/key-up events, same as lmb_down/rmb_down/mmb_down already
     * do for mouse buttons. */
    int   keys_down[PHI_KEY_COUNT];

    /* Relative mouse motion, accumulated since the last time player_
     * main.c drained it (same accumulate-then-drain convention scroll_
     * delta already uses) -- real, meaningful values ONLY while the
     * pointer is captured (see input_capture_mouse below); while
     * uncaptured this just mirrors ordinary mouse_x/y deltas, which a
     * caller generally shouldn't rely on (they're absolute-position-
     * derived, not raw device motion). Added for FPS-style mouse look
     * (2026-08-18) -- game/src/main.c's own first cut at this computed
     * deltas from mouse_x/y by hand, which breaks under real pointer
     * capture (native: the pointer gets warped back to center every
     * frame, so absolute position is meaningless; wasm: the browser
     * still reports it, but real relative motion -- movementX/Y -- is
     * both more correct and the standard way to read it). */
    int   mouse_dx, mouse_dy;
} InputState;

void input_init(InputState *inp);
void input_install_callbacks(InputState *inp);  /* registers JS event listeners */

/* Real pointer capture ("FPS mouse look") -- confines and hides the
 * cursor and switches mouse_dx/dy to real relative-motion values (see
 * that field's own comment). Added 2026-08-18 for the FPS demo (see
 * phi.md's Phase 9 notes) -- input.h's own top-of-struct comment already
 * mentions Qek's predecessor field this replaces (`pointer_locked`,
 * removed along with the rest of Qek's gameplay-specific InputState
 * fields when this became a UI-focused struct); this is a real,
 * deliberate re-addition for the player build's own genuine FPS-style
 * needs, not a resurrection of unused code.
 *   Native (X11): XGrabPointer confines the cursor to the window and a
 *     blank cursor hides it; mouse_dx/dy come from diffing each real
 *     XMotionEvent against window-center, then warping the pointer back
 *     to center (the standard technique -- the warp's own resulting
 *     MotionNotify lands exactly on center, contributing a harmless
 *     (0,0) to the next frame's accumulation, no special-casing needed).
 *   wasm: the browser's real Pointer Lock API (emscripten_request_
 *     pointerlock, deferred until the next in-page event so it's still
 *     honored despite not being called synchronously inside a user-
 *     gesture handler) -- mouse_dx/dy come from the DOM's own real
 *     movementX/movementY. Must be requested from within code that runs
 *     as a consequence of a real user gesture (a click) -- browsers
 *     refuse an unprompted pointer-lock request.
 * Not meaningful in a UI/panel context -- the editor never calls this.
 * Released automatically on Escape: natively, player_main.c calls
 * input_capture_mouse(0) when it sees escape_edge; in the browser, the
 * Pointer Lock API itself already exits on Escape with no code needed. */
void input_capture_mouse(int enable);
int  input_mouse_captured(void);

#ifndef __EMSCRIPTEN__
/* Called by phi_platform_native.c's event pump for every XEvent it
 * doesn't itself handle (resize/close). Takes `void *` rather than
 * `XEvent *` so this header doesn't need to pull in <X11/Xlib.h> for
 * callers that only ever see the wasm build. Real key/mouse/pointer-
 * capture handling lives in input.c's native branch. */
void input_native_handle_event(void *xevent);
#endif
