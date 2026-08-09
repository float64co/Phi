#include "input.h"
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>

static InputState *s_inp = NULL;

static void push_typed_char(InputState *inp, const char *key) {
    /* Only queue single printable ASCII/UTF8-byte chars — "Enter",
     * "Backspace", "Shift" etc. all have strlen(key) > 1. */
    if (key[0] != '\0' && key[1] == '\0' && inp->typed_count < TYPED_CHAR_QUEUE_SIZE) {
        inp->typed_chars[inp->typed_count++] = key[0];
    }
}

static EM_BOOL key_down(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;

    /* The Python panel is an always-focused text input (see console.c):
     * every keystroke is captured unconditionally, with no reserved
     * game-binding letters competing with it anymore. */
    push_typed_char(inp, e->key);
    if (!e->repeat) {
        if (strcmp(e->code,"Enter")==0 || strcmp(e->code,"NumpadEnter")==0) inp->enter_edge = 1;
        if (strcmp(e->code,"ArrowUp")==0)    inp->histup_edge   = 1;
        if (strcmp(e->code,"ArrowDown")==0)  inp->histdown_edge = 1;
    }
    if (strcmp(e->code,"Backspace")==0) inp->backspace_edge = 1;  /* natural OS repeat-delete */
    return EM_TRUE;
}

static EM_BOOL key_up(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    return EM_TRUE;
}

/* mousemove on DOCUMENT so panel/gizmo hit-testing keeps tracking the
 * cursor even when it briefly leaves the canvas element. */
static EM_BOOL mouse_move(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    inp->mouse_x = e->targetX;
    inp->mouse_y = e->targetY;
    return EM_TRUE;
}

static EM_BOOL mouse_down(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    inp->mouse_x = e->targetX;
    inp->mouse_y = e->targetY;
    if (e->button == 0)      { inp->lmb_down = 1; inp->lmb_click = 1; }
    else if (e->button == 2) { inp->rmb_down = 1; inp->rmb_click = 1; }
    return EM_TRUE;
}

static EM_BOOL mouse_up(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    if (e->button == 0) inp->lmb_down = 0;
    if (e->button == 2) inp->rmb_down = 0;
    return EM_TRUE;
}

/* Scroll wheel: currently unassigned. Its previous job (Sauerbraten-style
 * octree-editor grid cycling) went away with the Qek editor bindings; the
 * natural future assignment is Scene-panel camera zoom/dolly once real
 * editor navigation lands, which is why the handler shell stays
 * registered rather than being torn out. */
static EM_BOOL wheel_move(int type, const EmscriptenWheelEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    return EM_TRUE;
}

/* Clears held mouse-button state. Called whenever the browser might stop
 * delivering mouseup events to us (window loses focus) — otherwise a
 * button held at that exact moment sticks "on" forever (no matching
 * mouseup ever arrives), e.g. leaving a gizmo drag stuck active. */
static void reset_held_buttons(InputState *inp) {
    inp->lmb_down = inp->rmb_down = 0;
}

static EM_BOOL on_blur(int type, const EmscriptenFocusEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    if (s_inp) reset_held_buttons(s_inp);
    return EM_TRUE;
}

void input_install_callbacks(InputState *inp) {
    s_inp = inp;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, 1, key_down);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,     NULL, 1, key_up);
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, mouse_move);
    emscripten_set_mousedown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, mouse_down);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,   NULL, 1, mouse_up);
    emscripten_set_wheel_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,     NULL, 1, wheel_move);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,        NULL, 1, on_blur);
}

#elif defined(_WIN32)
#include <windows.h>

/* Mirrors the X11 branch below one message at a time, driven by
 * phi_platform_win32.c's WndProc via input_native_handle_event() — same
 * role as the X11 event pump, message-driven instead of pumped. */

static InputState *s_inp  = NULL;

static void reset_held_buttons_win32(InputState *inp) {
    inp->lmb_down = inp->rmb_down = 0;
}

void input_install_callbacks(InputState *inp) {
    s_inp = inp;
}

static void handle_key(WPARAM vk, LPARAM lparam, int down) {
    InputState *inp = s_inp;
    if (!inp) return;
    /* Bit 30 of WM_KEYDOWN's lParam is the previous key state (1 = was
     * already down) — Windows hands us the repeat flag directly, unlike
     * X11 which needs XkbSetDetectableAutoRepeat + manual per-keycode
     * state tracking to get the same information. */
    int was_down = (lparam & (1 << 30)) != 0;
    int edge = down && !was_down;

    if (down && edge) {
        if (vk == VK_RETURN) inp->enter_edge     = 1;
        if (vk == VK_UP)     inp->histup_edge    = 1;
        if (vk == VK_DOWN)   inp->histdown_edge  = 1;
    }
    if (down && vk == VK_BACK) inp->backspace_edge = 1;  /* natural OS repeat-delete */
}

/* WM_CHAR gives the actual translated character (handles shift/layout the
 * same way X11's XLookupString does) — separate from WM_KEYDOWN's virtual-
 * key-code-based handling above, matching the X11 branch's split between
 * typed-char capture and edge-triggered keys. */
static void handle_char(WPARAM wparam) {
    InputState *inp = s_inp;
    if (!inp) return;
    if (wparam >= 32 && wparam < 127 && inp->typed_count < TYPED_CHAR_QUEUE_SIZE)
        inp->typed_chars[inp->typed_count++] = (char)wparam;
}

static void handle_motion(LPARAM lparam) {
    InputState *inp = s_inp;
    if (!inp) return;
    inp->mouse_x = (short)LOWORD(lparam);  /* client-area coords, like X11's ev->xmotion.x/y */
    inp->mouse_y = (short)HIWORD(lparam);
}

static void handle_button(int is_right, int down, LPARAM lparam) {
    InputState *inp = s_inp;
    if (!inp) return;
    inp->mouse_x = (short)LOWORD(lparam);
    inp->mouse_y = (short)HIWORD(lparam);
    if (!is_right) {
        inp->lmb_down = down;
        if (down) inp->lmb_click = 1;
    } else {
        inp->rmb_down = down;
        if (down) inp->rmb_click = 1;
    }
}

static void handle_wheel(WPARAM wparam) {
    /* Unassigned -- same reasoning as the wasm branch's wheel_move. */
    (void)wparam;
}

void input_native_handle_event(void *msgptr) {
    MSG *m = (MSG *)msgptr;
    switch (m->message) {
        case WM_KEYDOWN:     handle_key(m->wParam, m->lParam, 1); break;
        case WM_KEYUP:       handle_key(m->wParam, m->lParam, 0); break;
        case WM_CHAR:        handle_char(m->wParam);              break;
        case WM_MOUSEMOVE:   handle_motion(m->lParam);            break;
        case WM_LBUTTONDOWN: handle_button(0, 1, m->lParam);      break;
        case WM_LBUTTONUP:   handle_button(0, 0, m->lParam);      break;
        case WM_RBUTTONDOWN: handle_button(1, 1, m->lParam);      break;
        case WM_RBUTTONUP:   handle_button(1, 0, m->lParam);      break;
        case WM_MOUSEWHEEL:  handle_wheel(m->wParam);              break;
        case WM_KILLFOCUS:   if (s_inp) reset_held_buttons_win32(s_inp); break;
        default: break;
    }
}

#else
#include "phi_platform_native.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>   /* XLookupString */
#include <X11/keysym.h>
#include <X11/XKBlib.h>

/* Mirrors the Emscripten key_down/key_up/mouse_move/mouse_down/mouse_up/
 * wheel_move functions above, one XEvent at a time, driven by
 * phi_platform_native.c's event pump via input_native_handle_event(). */

static InputState *s_inp = NULL;
static int         s_key_down[256];   /* indexed by raw X11 keycode, for edge detection below */

static void reset_held_buttons_native(InputState *inp) {
    inp->lmb_down = inp->rmb_down = 0;
}

void input_install_callbacks(InputState *inp) {
    s_inp = inp;
    memset(s_key_down, 0, sizeof(s_key_down));
    /* Without this, X sends a synthetic KeyRelease before every repeated
     * KeyPress while a key is held, making a genuine press indistinguishable
     * from OS auto-repeat -- Enter/Up/Down below are edge-triggered
     * (submit-once, step-history-once), so without detectable autorepeat a
     * held Enter would resubmit the console line on every repeat tick. */
    XkbSetDetectableAutoRepeat(phi_platform_native_display(), True, NULL);
}

static void handle_key(XKeyEvent *e, int down) {
    InputState *inp = s_inp;
    if (!inp) return;
    unsigned int kc = e->keycode & 0xFF;
    int edge = down && !s_key_down[kc];
    s_key_down[kc] = down;

    KeySym ks = XLookupKeysym(e, 0);

    /* Python-panel text entry: always-focused, no reserved game-binding
     * letters competing with typing anymore (see the wasm branch's
     * key_down for the same reasoning). */
    if (down) {
        char buf[8]; KeySym dummy;
        int n = XLookupString(e, buf, sizeof(buf) - 1, &dummy, NULL);
        if (n == 1 && buf[0] >= 32 && buf[0] < 127 && inp->typed_count < TYPED_CHAR_QUEUE_SIZE)
            inp->typed_chars[inp->typed_count++] = buf[0];

        if (edge) {
            if (ks == XK_Return || ks == XK_KP_Enter)  inp->enter_edge    = 1;
            if (ks == XK_Up)                            inp->histup_edge   = 1;
            if (ks == XK_Down)                          inp->histdown_edge = 1;
        }
        if (ks == XK_BackSpace) inp->backspace_edge = 1;  /* natural OS repeat-delete */
    }
}

static void handle_motion(XMotionEvent *e) {
    InputState *inp = s_inp;
    if (!inp) return;
    inp->mouse_x = e->x;
    inp->mouse_y = e->y;
}

static void handle_button(XButtonEvent *e, int down) {
    InputState *inp = s_inp;
    if (!inp) return;
    inp->mouse_x = e->x;
    inp->mouse_y = e->y;
    if (e->button == Button1) {
        inp->lmb_down = down;
        if (down) inp->lmb_click = 1;
    } else if (e->button == Button3) {
        inp->rmb_down = down;
        if (down) inp->rmb_click = 1;
    }
    /* Button4/Button5 (scroll) are unassigned, same reasoning as the wasm
     * branch's wheel_move. */
}

void input_native_handle_event(void *xevent) {
    XEvent *ev = (XEvent *)xevent;
    switch (ev->type) {
        case KeyPress:      handle_key(&ev->xkey, 1);    break;
        case KeyRelease:    handle_key(&ev->xkey, 0);    break;
        case MotionNotify:  handle_motion(&ev->xmotion);  break;
        case ButtonPress:   handle_button(&ev->xbutton, 1); break;
        case ButtonRelease: handle_button(&ev->xbutton, 0); break;
        case FocusOut:      if (s_inp) reset_held_buttons_native(s_inp); break;
        default: break;
    }
}
#endif

void input_init(InputState *inp) {
    memset(inp, 0, sizeof(*inp));
}
