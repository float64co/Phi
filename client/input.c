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

#else
void input_install_callbacks(InputState *inp) { (void)inp; }
void input_set_pointer_locked(int locked)      { (void)locked; }
/* Real key/mouse translation lands with native input support; until then
 * the native build renders with a stationary, uncontrollable camera. */
void input_native_handle_event(void *xevent) { (void)xevent; }
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
