#include "input.h"
#include <string.h>
#include <math.h>
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

    /* ---- Console text entry: captured unconditionally ---- */
    push_typed_char(inp, e->key);
    if (!e->repeat) {
        if (strcmp(e->code,"Backquote")==0) inp->console_toggle = 1;
        if (strcmp(e->code,"Enter")==0 || strcmp(e->code,"NumpadEnter")==0) inp->enter_edge = 1;
        if (strcmp(e->code,"Escape")==0)     inp->escape_edge   = 1;
        if (strcmp(e->code,"ArrowUp")==0)    inp->histup_edge   = 1;
        if (strcmp(e->code,"ArrowDown")==0)  inp->histdown_edge = 1;
    }
    if (strcmp(e->code,"Backspace")==0) inp->backspace_edge = 1;  /* natural OS repeat-delete */

    /* ---- Game / editor bindings: suppressed while typing ---- */
    if (!inp->console_open) {
        if (strcmp(e->code,"KeyW")==0||strcmp(e->code,"ArrowUp")==0)    inp->forward=1;
        if (strcmp(e->code,"KeyS")==0||strcmp(e->code,"ArrowDown")==0)  inp->back=1;
        if (strcmp(e->code,"KeyA")==0||strcmp(e->code,"ArrowLeft")==0)  inp->left=1;
        if (strcmp(e->code,"KeyD")==0||strcmp(e->code,"ArrowRight")==0) inp->right=1;
        if (strcmp(e->code,"Space")==0)                                  inp->jump=1;
        if (strcmp(e->code,"F4")==0)                                     inp->export_stl=1;
        if (strcmp(e->code,"KeyX")==0)                                   inp->up=1;
        if (strcmp(e->code,"KeyZ")==0)                                   inp->down=1;
        if (strcmp(e->code,"KeyM")==0)                                   inp->paint_mod=1;
        if (strcmp(e->code,"ShiftLeft")==0||strcmp(e->code,"ShiftRight")==0) inp->shift=1;
        if (strcmp(e->code,"KeyC")==0) inp->crouch=1;
        if (!e->repeat) {
            if (strcmp(e->code,"KeyE")==0)          inp->edit_toggle = 1;
            if (strcmp(e->code,"BracketLeft")==0)   inp->grid_dec    = 1;
            if (strcmp(e->code,"BracketRight")==0)  inp->grid_inc    = 1;
            if (strcmp(e->code,"Comma")==0)         inp->mat_dec     = 1;
            if (strcmp(e->code,"Period")==0)        inp->mat_inc     = 1;

            /* console 'bind' — record every non-repeat keypress code so
             * console.c can match it against user-defined binds */
            if (inp->pressed_code_count < PRESSED_CODE_QUEUE_SIZE) {
                strncpy(inp->pressed_codes[inp->pressed_code_count], e->code, KEY_CODE_LEN - 1);
                inp->pressed_codes[inp->pressed_code_count][KEY_CODE_LEN - 1] = 0;
                inp->pressed_code_count++;
            }
        }
    }
    return EM_TRUE;
}

static EM_BOOL key_up(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    /* Always clear held state unconditionally, even if console_open toggled
     * mid-hold — otherwise a key released while typing would stick. */
    if (strcmp(e->code,"KeyW")==0||strcmp(e->code,"ArrowUp")==0)    inp->forward=0;
    if (strcmp(e->code,"KeyS")==0||strcmp(e->code,"ArrowDown")==0)  inp->back=0;
    if (strcmp(e->code,"KeyA")==0||strcmp(e->code,"ArrowLeft")==0)  inp->left=0;
    if (strcmp(e->code,"KeyD")==0||strcmp(e->code,"ArrowRight")==0) inp->right=0;
    if (strcmp(e->code,"Space")==0)                                  inp->jump=0;
    if (strcmp(e->code,"KeyX")==0)                                   inp->up=0;
    if (strcmp(e->code,"KeyZ")==0)                                   inp->down=0;
    if (strcmp(e->code,"KeyM")==0)                                   inp->paint_mod=0;
    if (strcmp(e->code,"ShiftLeft")==0||strcmp(e->code,"ShiftRight")==0) inp->shift=0;
    if (strcmp(e->code,"KeyC")==0) inp->crouch=0;
    return EM_TRUE;
}

/* mousemove on DOCUMENT — pointer lock sends events here, not to canvas */
static EM_BOOL mouse_move(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp || !inp->pointer_locked) return EM_FALSE;
    inp->yaw   -= (float)e->movementX * inp->sensitivity;
    inp->pitch -= (float)e->movementY * inp->sensitivity;
    float limit = 89.0f * (float)M_PI / 180.0f;
    if (inp->pitch >  limit) inp->pitch =  limit;
    if (inp->pitch < -limit) inp->pitch = -limit;
    return EM_TRUE;
}

static EM_BOOL mouse_down(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    if (!inp->pointer_locked || inp->console_open) return EM_TRUE;
    if (e->button == 0) {
        if (!inp->fire_held) { inp->fire = 1; inp->fire_held = 1; }
        inp->lmb_down = 1;
        /* Pointer lock request is handled entirely from JS (index.html) */
    } else if (e->button == 2) {
        inp->rmb_down = 1;
    }
    return EM_TRUE;
}

static EM_BOOL mouse_up(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    if (e->button == 0) { inp->fire_held = 0; inp->lmb_down = 0; }
    if (e->button == 2)   inp->rmb_down = 0;
    return EM_TRUE;
}

/* Sauerbraten convention: scroll wheel cycles editor grid size — same
 * action as the [ / ] keys, just another way to reach grid_inc/grid_dec. */
static EM_BOOL wheel_move(int type, const EmscriptenWheelEvent *e, void *ud) {
    (void)type; (void)ud;
    InputState *inp = s_inp;
    if (!inp) return EM_FALSE;
    if (!inp->pointer_locked || inp->console_open) return EM_TRUE;
    if (e->deltaY < 0)      inp->grid_inc = 1;
    else if (e->deltaY > 0) inp->grid_dec = 1;
    return EM_TRUE;
}

/* Zero every "held" input state. Called whenever the browser might stop
 * delivering keyup/mouseup events to us (pointer lock lost, window loses
 * focus) — otherwise a key held at that exact moment sticks "on" forever
 * (no matching keyup ever arrives), and the player just keeps sliding in
 * that direction indefinitely. */
static void reset_held_keys(InputState *inp) {
    inp->forward = inp->back = inp->left = inp->right = 0;
    inp->jump = inp->up = inp->down = inp->paint_mod = inp->shift = inp->crouch = 0;
    inp->fire = inp->fire_held = 0;
    inp->lmb_down = inp->rmb_down = 0;
}

static EM_BOOL on_blur(int type, const EmscriptenFocusEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    if (s_inp) reset_held_keys(s_inp);
    return EM_TRUE;
}

void input_install_callbacks(InputState *inp) {
    s_inp = inp;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, 1, key_down);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,     NULL, 1, key_up);
    /* mousemove on document so it fires while pointer is locked */
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, mouse_move);
    emscripten_set_mousedown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, mouse_down);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,   NULL, 1, mouse_up);
    emscripten_set_wheel_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT,     NULL, 1, wheel_move);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,        NULL, 1, on_blur);
}

/* Called from JS when pointer lock state changes */
EMSCRIPTEN_KEEPALIVE
void input_set_pointer_locked(int locked) {
    if (s_inp) {
        s_inp->pointer_locked = locked;
        if (!locked) reset_held_keys(s_inp);
        printf("[input] pointer_locked=%d\n", locked);
    }
}

#elif defined(_WIN32)
#include "phi_platform.h"
#include "phi_platform_win32.h"
#include <windows.h>

/* Mirrors the X11 branch below one message at a time, driven by
 * phi_platform_win32.c's WndProc via input_native_handle_event() — same
 * role as the X11 event pump, message-driven instead of pumped.
 *
 * Known gap, same as X11: the console `bind` system's pressed_codes[]
 * (browser KeyboardEvent .code strings) is left empty here — `bind` is a
 * no-op on native for now rather than half-mapping VK codes to those
 * string names. */

static InputState *s_inp  = NULL;
static HWND         s_hwnd = NULL;
static int          s_ignore_next_motion = 0;

static void reset_held_keys_win32(InputState *inp) {
    inp->forward = inp->back = inp->left = inp->right = 0;
    inp->jump = inp->up = inp->down = inp->paint_mod = inp->shift = inp->crouch = 0;
    inp->fire = inp->fire_held = 0;
    inp->lmb_down = inp->rmb_down = 0;
}

static void warp_to_center(void) {
    int w, h; phi_platform_get_window_size(&w, &h);
    POINT pt = { w / 2, h / 2 };
    ClientToScreen(s_hwnd, &pt);
    SetCursorPos(pt.x, pt.y);
    s_ignore_next_motion = 1;
}

void input_install_callbacks(InputState *inp) {
    s_inp  = inp;
    s_hwnd = phi_platform_win32_window();

    /* Deliberately NOT hiding/confining the cursor here anymore -- see the
     * matching comment in the native (X11) branch's input_install_callbacks
     * for why. Phi is an editor now; ShowCursor(FALSE) + ClipCursor
     * unconditionally at startup made the panel/menu/outliner chrome
     * unclickable. pointer_locked stays 0 (see handle_motion below); an
     * opt-in engage/release gesture is follow-up work, not done here. */
    inp->pointer_locked = 0;
}

void input_set_pointer_locked(int locked) {
    if (s_inp) {
        s_inp->pointer_locked = locked;
        if (!locked) reset_held_keys_win32(s_inp);
    }
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
        if (vk == VK_OEM_3)  inp->console_toggle = 1;   /* ` */
        if (vk == VK_RETURN) inp->enter_edge     = 1;
        if (vk == VK_ESCAPE) inp->escape_edge    = 1;
        if (vk == VK_UP)     inp->histup_edge    = 1;
        if (vk == VK_DOWN)   inp->histdown_edge  = 1;
    }
    if (down && vk == VK_BACK) inp->backspace_edge = 1;  /* natural OS repeat-delete */

    if (!inp->console_open) {
        int state = down ? 1 : 0;
        if (vk=='W' || vk==VK_UP)    inp->forward = state;
        if (vk=='S' || vk==VK_DOWN)  inp->back    = state;
        if (vk=='A' || vk==VK_LEFT)  inp->left    = state;
        if (vk=='D' || vk==VK_RIGHT) inp->right   = state;
        if (vk==VK_SPACE)            inp->jump    = state;
        if (vk=='X')                 inp->up      = state;
        if (vk=='Z')                 inp->down    = state;
        if (vk=='M')                 inp->paint_mod = state;
        if (vk==VK_SHIFT)            inp->shift   = state;
        if (vk=='C')                 inp->crouch  = state;
        if (down && vk==VK_F4)       inp->export_stl = 1;

        if (edge) {
            if (vk=='E')             inp->edit_toggle = 1;
            if (vk==VK_OEM_4)        inp->grid_dec = 1;   /* [ */
            if (vk==VK_OEM_6)        inp->grid_inc = 1;   /* ] */
            if (vk==VK_OEM_COMMA)    inp->mat_dec  = 1;
            if (vk==VK_OEM_PERIOD)   inp->mat_inc  = 1;
        }
    }
}

/* WM_CHAR gives the actual translated character (handles shift/layout the
 * same way X11's XLookupString does) — separate from WM_KEYDOWN's virtual-
 * key-code-based game-binding handling above, matching the X11 branch's
 * split between typed-char capture and game bindings. */
static void handle_char(WPARAM wparam) {
    InputState *inp = s_inp;
    if (!inp) return;
    if (wparam >= 32 && wparam < 127 && inp->typed_count < TYPED_CHAR_QUEUE_SIZE)
        inp->typed_chars[inp->typed_count++] = (char)wparam;
}

static void handle_motion(LPARAM lparam) {
    InputState *inp = s_inp;
    if (!inp || !inp->pointer_locked) return;  /* no lock engaged -- let the cursor move freely, don't warp it back */
    if (s_ignore_next_motion) { s_ignore_next_motion = 0; return; }

    int x = (short)LOWORD(lparam);  /* client-area coords, like X11's ev->xmotion.x/y */
    int y = (short)HIWORD(lparam);
    int w, h; phi_platform_get_window_size(&w, &h);
    int dx = x - w / 2, dy = y - h / 2;
    if (dx == 0 && dy == 0) return;

    inp->yaw   -= (float)dx * inp->sensitivity;
    inp->pitch -= (float)dy * inp->sensitivity;
    float limit = 89.0f * (float)M_PI / 180.0f;
    if (inp->pitch >  limit) inp->pitch =  limit;
    if (inp->pitch < -limit) inp->pitch = -limit;

    warp_to_center();
}

static void handle_button(int is_right, int down) {
    InputState *inp = s_inp;
    if (!inp || inp->console_open) return;
    if (!is_right) {
        if (down) { if (!inp->fire_held) { inp->fire = 1; inp->fire_held = 1; } inp->lmb_down = 1; }
        else      { inp->fire_held = 0; inp->lmb_down = 0; }
    } else {
        inp->rmb_down = down;
    }
}

static void handle_wheel(WPARAM wparam) {
    InputState *inp = s_inp;
    if (!inp || inp->console_open) return;
    short delta = (short)HIWORD(wparam);  /* signed, +120/-120 per notch */
    if (delta > 0)      inp->grid_inc = 1;   /* scroll up — matches wheel_move's convention */
    else if (delta < 0) inp->grid_dec = 1;
}

void input_native_handle_event(void *msgptr) {
    MSG *m = (MSG *)msgptr;
    switch (m->message) {
        case WM_KEYDOWN:     handle_key(m->wParam, m->lParam, 1); break;
        case WM_KEYUP:       handle_key(m->wParam, m->lParam, 0); break;
        case WM_CHAR:        handle_char(m->wParam);              break;
        case WM_MOUSEMOVE:   handle_motion(m->lParam);            break;
        case WM_LBUTTONDOWN: handle_button(0, 1);                 break;
        case WM_LBUTTONUP:   handle_button(0, 0);                 break;
        case WM_RBUTTONDOWN: handle_button(1, 1);                 break;
        case WM_RBUTTONUP:   handle_button(1, 0);                 break;
        case WM_MOUSEWHEEL:  handle_wheel(m->wParam);              break;
        case WM_KILLFOCUS:   if (s_inp) reset_held_keys_win32(s_inp); break;
        default: break;
    }
}

#else
#include "phi_platform.h"
#include "phi_platform_native.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>   /* XLookupString */
#include <X11/keysym.h>
#include <X11/XKBlib.h>

/* Mirrors the Emscripten key_down/key_up/mouse_move/mouse_down/mouse_up/
 * wheel_move functions above, one XEvent at a time, driven by
 * phi_platform_native.c's event pump via input_native_handle_event().
 *
 * Known gap: the console `bind` system's pressed_codes[] (browser KeyboardEvent
 * .code strings like "KeyG") is left empty here — `bind` is a no-op on native
 * for now rather than half-mapping keysyms to those string names. */

static InputState *s_inp = NULL;
static Display     *s_dpy = NULL;
static Window        s_win;
static int           s_key_down[256];       /* indexed by raw X11 keycode */
static int           s_ignore_next_motion = 0;

static void reset_held_keys_native(InputState *inp) {
    inp->forward = inp->back = inp->left = inp->right = 0;
    inp->jump = inp->up = inp->down = inp->paint_mod = inp->shift = inp->crouch = 0;
    inp->fire = inp->fire_held = 0;
    inp->lmb_down = inp->rmb_down = 0;
}

static void warp_to_center(void) {
    int w, h; phi_platform_get_window_size(&w, &h);
    XWarpPointer(s_dpy, None, s_win, 0, 0, 0, 0, w / 2, h / 2);
    s_ignore_next_motion = 1;
}

void input_install_callbacks(InputState *inp) {
    s_inp = inp;
    s_dpy = phi_platform_native_display();
    s_win = phi_platform_native_window();
    memset(s_key_down, 0, sizeof(s_key_down));

    /* Without this, X sends a synthetic KeyRelease before every repeated
     * KeyPress while a key is held, making held-key state indistinguishable
     * from a real release+press — breaks WASD and edge-detection alike. */
    XkbSetDetectableAutoRepeat(s_dpy, True, NULL);

    /* Deliberately NOT grabbing/hiding/warping the pointer here anymore.
     * This used to run unconditionally at startup (XGrabPointer + an
     * invisible cursor + a center warp every motion event), matching
     * Qek's always-on FPS mouse-look -- but Phi is an editor now, and a
     * confined, invisible, snap-to-center cursor makes it impossible to
     * actually click any of the rendered panel/menu/outliner chrome. No
     * click-to-engage gesture (mirroring the web build's canvas-click ->
     * pointer lock) exists on native yet -- pointer_locked simply stays 0
     * (see handle_motion below), leaving the cursor free for normal UI
     * use; wiring up an opt-in engage/release gesture is follow-up work,
     * not done here. */
    inp->pointer_locked = 0;
}

void input_set_pointer_locked(int locked) {
    if (s_inp) {
        s_inp->pointer_locked = locked;
        if (!locked) reset_held_keys_native(s_inp);
    }
}

static void handle_key(XKeyEvent *e, int down) {
    InputState *inp = s_inp;
    if (!inp) return;
    unsigned int kc = e->keycode & 0xFF;
    int edge = down && !s_key_down[kc];
    s_key_down[kc] = down;

    KeySym ks = XLookupKeysym(e, 0);

    /* ---- Console text entry: captured unconditionally ---- */
    if (down) {
        char buf[8]; KeySym dummy;
        int n = XLookupString(e, buf, sizeof(buf) - 1, &dummy, NULL);
        if (n == 1 && buf[0] >= 32 && buf[0] < 127 && inp->typed_count < TYPED_CHAR_QUEUE_SIZE)
            inp->typed_chars[inp->typed_count++] = buf[0];

        if (edge) {
            if (ks == XK_grave)                       inp->console_toggle = 1;
            if (ks == XK_Return || ks == XK_KP_Enter)  inp->enter_edge    = 1;
            if (ks == XK_Escape)                       inp->escape_edge   = 1;
            if (ks == XK_Up)                            inp->histup_edge   = 1;
            if (ks == XK_Down)                          inp->histdown_edge = 1;
        }
        if (ks == XK_BackSpace) inp->backspace_edge = 1;  /* natural OS repeat-delete */
    }

    /* ---- Game / editor bindings: suppressed while typing ---- */
    if (!inp->console_open) {
        int state = down ? 1 : 0;
        if (ks==XK_w || ks==XK_W || ks==XK_Up)    inp->forward = state;
        if (ks==XK_s || ks==XK_S || ks==XK_Down)  inp->back    = state;
        if (ks==XK_a || ks==XK_A || ks==XK_Left)  inp->left    = state;
        if (ks==XK_d || ks==XK_D || ks==XK_Right) inp->right   = state;
        if (ks==XK_space)                         inp->jump    = state;
        if (ks==XK_x || ks==XK_X)                 inp->up      = state;
        if (ks==XK_z || ks==XK_Z)                 inp->down    = state;
        if (ks==XK_m || ks==XK_M)                 inp->paint_mod = state;
        if (ks==XK_Shift_L || ks==XK_Shift_R)     inp->shift   = state;
        if (ks==XK_c || ks==XK_C)                 inp->crouch  = state;
        if (down && ks==XK_F4)                    inp->export_stl = 1;

        if (edge) {
            if (ks==XK_e || ks==XK_E)   inp->edit_toggle = 1;
            if (ks==XK_bracketleft)     inp->grid_dec    = 1;
            if (ks==XK_bracketright)    inp->grid_inc    = 1;
            if (ks==XK_comma)           inp->mat_dec     = 1;
            if (ks==XK_period)          inp->mat_inc     = 1;
        }
    }
}

static void handle_motion(XMotionEvent *e) {
    InputState *inp = s_inp;
    if (!inp || !inp->pointer_locked) return;  /* no lock engaged -- let the cursor move freely, don't warp it back */
    if (s_ignore_next_motion) { s_ignore_next_motion = 0; return; }

    int w, h; phi_platform_get_window_size(&w, &h);
    int dx = e->x - w / 2, dy = e->y - h / 2;
    if (dx == 0 && dy == 0) return;

    inp->yaw   -= (float)dx * inp->sensitivity;
    inp->pitch -= (float)dy * inp->sensitivity;
    float limit = 89.0f * (float)M_PI / 180.0f;
    if (inp->pitch >  limit) inp->pitch =  limit;
    if (inp->pitch < -limit) inp->pitch = -limit;

    warp_to_center();
}

static void handle_button(XButtonEvent *e, int down) {
    InputState *inp = s_inp;
    if (!inp || inp->console_open) return;
    if (e->button == Button1) {
        if (down) { if (!inp->fire_held) { inp->fire = 1; inp->fire_held = 1; } inp->lmb_down = 1; }
        else      { inp->fire_held = 0; inp->lmb_down = 0; }
    } else if (e->button == Button3) {
        inp->rmb_down = down;
    } else if (down && e->button == Button4) {
        inp->grid_inc = 1;   /* scroll up — same convention as wheel_move */
    } else if (down && e->button == Button5) {
        inp->grid_dec = 1;   /* scroll down */
    }
}

void input_native_handle_event(void *xevent) {
    XEvent *ev = (XEvent *)xevent;
    switch (ev->type) {
        case KeyPress:      handle_key(&ev->xkey, 1);    break;
        case KeyRelease:    handle_key(&ev->xkey, 0);    break;
        case MotionNotify:  handle_motion(&ev->xmotion);  break;
        case ButtonPress:   handle_button(&ev->xbutton, 1); break;
        case ButtonRelease: handle_button(&ev->xbutton, 0); break;
        case FocusOut:      if (s_inp) reset_held_keys_native(s_inp); break;
        default: break;
    }
}
#endif

void input_init(InputState *inp) {
    memset(inp, 0, sizeof(*inp));
    inp->sensitivity   = 0.002f;
    inp->yaw           = 0.0f;
    inp->pitch         = 0.0f;
    inp->pointer_locked = 0;
}

void input_set_console_open(InputState *inp, int open) {
    inp->console_open = open;
    /* Closing/opening mid-hold shouldn't leave movement stuck */
    if (open) {
        inp->forward = inp->back = inp->left = inp->right = 0;
        inp->jump = inp->up = inp->down = inp->paint_mod = inp->shift = inp->crouch = 0;
    }
}

uint8_t input_get_key_flags(const InputState *inp) {
    uint8_t flags = 0;
    if (inp->forward)  flags |= KEY_FORWARD;
    if (inp->back)     flags |= KEY_BACK;
    if (inp->left)     flags |= KEY_LEFT;
    if (inp->right)    flags |= KEY_RIGHT;
    if (inp->jump)     flags |= KEY_JUMP;
    if (inp->fire)     flags |= KEY_FIRE;
    return flags;
}
