#pragma once
#include <stdint.h>

/* Key bit flags for net_send_input */
#define KEY_FORWARD  (1<<0)
#define KEY_BACK     (1<<1)
#define KEY_LEFT     (1<<2)
#define KEY_RIGHT    (1<<3)
#define KEY_JUMP     (1<<4)
#define KEY_FIRE     (1<<5)
#define KEY_EDITING  (1<<6)   /* set by main.c (mirrors EditorState.active) so the
                                * server can make this player immune while editing */
#define KEY_GOD      (1<<7)   /* set by main.c (mirrors Player.god) so the server
                                * also honors the 'god' console command */

#define TYPED_CHAR_QUEUE_SIZE 32

typedef struct {
    int   forward, back, left, right, jump;
    int   fire;           /* rising edge */
    int   fire_held;
    int   export_stl;     /* F4 key */
    float yaw;
    float pitch;
    float sensitivity;
    int   pointer_locked;
    uint16_t input_seq;

    /* ---- Editor / console ----
     * console_open mirrors ConsoleState.open (set via
     * input_set_console_open(), same pattern as pointer_locked) so
     * key_down/key_up can gate WASD/E/mouse game-bindings while the
     * console is capturing text. */
    int   console_open;

    int   edit_toggle;      /* E, rising edge */
    int   console_toggle;   /* ` (Backquote), rising edge, never gated */

    int   up, down;         /* noclip fly in editor: X / Z, held */
    int   paint_mod;        /* M held: LMB drag repaints material instead of geometry */
    int   shift;            /* Shift held: sprint multiplier on editor fly speed */
    int   crouch;           /* Ctrl held: crouch (normal gameplay, not editor) */

    int   lmb_down, rmb_down;  /* raw mouse button state while pointer-locked */

    /* Absolute window-local cursor position (y-down), tracked unconditionally
     * on every mouse-move regardless of pointer_locked -- unlike lmb_down/
     * fire above (gameplay-specific, still gated behind pointer_locked/
     * console_open), UI panel hit-testing (ui_on_mouse_button) needs a real
     * cursor position and works whether or not the FPS camera is locked. */
    int   mouse_x, mouse_y;
    /* Rising edge: left/right mouse button pressed this frame, at
     * (mouse_x, mouse_y) when read -- same "main.c drains and clears it"
     * convention as fire/export_stl. Distinct from lmb_down/rmb_down (held
     * state, gameplay) and fire (gameplay-specific edge, still gated behind
     * pointer_locked) -- these two exist purely to route real clicks into
     * ui_on_mouse_button(), gated only by console_open. */
    int   lmb_click, rmb_click;

    int   grid_inc, grid_dec;  /* ] / [, rising edge */
    int   mat_inc, mat_dec;    /* . / , , rising edge */

    /* Console text entry — input.c captures these unconditionally each
     * frame; console.c decides whether to consume them. */
    char  typed_chars[TYPED_CHAR_QUEUE_SIZE];
    int   typed_count;
    int   enter_edge, backspace_edge, escape_edge;
    int   histup_edge, histdown_edge;   /* Up / Down arrows: console history */
} InputState;

void input_init(InputState *inp);
void input_install_callbacks(InputState *inp);  /* registers JS event listeners */
uint8_t input_get_key_flags(const InputState *inp);
/* Called from JS when pointer lock state changes */
void input_set_pointer_locked(int locked);
/* Called from console.c/main.c whenever the console open/close state changes */
void input_set_console_open(InputState *inp, int open);

#ifndef __EMSCRIPTEN__
/* Called by phi_platform_native.c's event pump for every XEvent it doesn't
 * itself handle (resize/close). Takes `void *` rather than `XEvent *` so
 * this header doesn't need to pull in <X11/Xlib.h> for callers that only
 * ever see the wasm build. No-op today — real key/mouse translation is a
 * separate, not-yet-landed piece of native input support. */
void input_native_handle_event(void *xevent);
#endif
