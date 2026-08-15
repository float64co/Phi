/* wasm backend for input_gamepad.h -- Emscripten's own native Gamepad
 * API bindings (emscripten/html5.h), backed directly by the browser's
 * standard HTML5 Gamepad API. Deliberately NOT SDL2 here at all: unlike
 * native (client/input_gamepad_native.c, real vendored SDL2), Emscripten
 * doesn't need SDL2 as an intermediary to reach the browser's Gamepad
 * API -- the browser already normalizes standard-layout controllers
 * itself (the W3C "Standard Gamepad" mapping), and Emscripten exposes
 * that directly. Going through Emscripten's own SDL2-compatibility
 * shim instead would add a real extra translation layer for no benefit
 * on this target. No WebUSB involved anywhere in this path either --
 * ordinary Gamepad API, same as every browser game already uses. */
#include "input_gamepad.h"
#include <emscripten/html5.h>
#include <string.h>
#include <stdio.h>

static PhiGamepadState s_state[PHI_GAMEPAD_MAX];

void phi_gamepad_init(void) {
    memset(s_state, 0, sizeof(s_state));
    printf("[input_gamepad] wasm: using Emscripten's native Gamepad API bindings\n");
}

void phi_gamepad_shutdown(void) {
    memset(s_state, 0, sizeof(s_state));
}

/* W3C Standard Gamepad button index -> PhiGamepadButton, for controllers
 * the browser reports mapping=="standard" for (the vast majority in
 * practice). Indices 6/7 (analog triggers) are read from analogButton
 * directly further down, not through this table -- see phi_gamepad_poll.
 * A non-"standard"-mapped controller is reported as disconnected here
 * rather than guessed at -- a real, honest scope limit (see input_
 * gamepad.h's own comment), not a silently-wrong button mapping. */
static const int STANDARD_BUTTON_MAP[16] = {
    PHI_GAMEPAD_BUTTON_A, PHI_GAMEPAD_BUTTON_B, PHI_GAMEPAD_BUTTON_X, PHI_GAMEPAD_BUTTON_Y,
    PHI_GAMEPAD_BUTTON_LEFTSHOULDER, PHI_GAMEPAD_BUTTON_RIGHTSHOULDER,
    -1, -1,   /* 6,7 = analog triggers -- handled via analogButton, not digitalButton */
    PHI_GAMEPAD_BUTTON_BACK, PHI_GAMEPAD_BUTTON_START,
    PHI_GAMEPAD_BUTTON_LEFTSTICK, PHI_GAMEPAD_BUTTON_RIGHTSTICK,
    PHI_GAMEPAD_BUTTON_DPAD_UP, PHI_GAMEPAD_BUTTON_DPAD_DOWN, PHI_GAMEPAD_BUTTON_DPAD_LEFT, PHI_GAMEPAD_BUTTON_DPAD_RIGHT,
};

void phi_gamepad_poll(void) {
    if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) {
        /* No Gamepad API support in this browser, or no permission yet --
         * a real, unsurprising outcome, not an error. Report everything
         * disconnected rather than raising/crashing. */
        for (int i = 0; i < PHI_GAMEPAD_MAX; i++) s_state[i].connected = 0;
        return;
    }

    int n = emscripten_get_num_gamepads();
    if (n < 0) n = 0;
    if (n > PHI_GAMEPAD_MAX) n = PHI_GAMEPAD_MAX;

    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
        if (i >= n) { s_state[i].connected = 0; continue; }

        EmscriptenGamepadEvent ev;
        if (emscripten_get_gamepad_status(i, &ev) != EMSCRIPTEN_RESULT_SUCCESS || !ev.connected) {
            s_state[i].connected = 0;
            continue;
        }
        if (strcmp(ev.mapping, "standard") != 0) {
            s_state[i].connected = 0;
            continue;
        }

        s_state[i].connected = 1;
        strncpy(s_state[i].name, ev.id, PHI_GAMEPAD_NAME_LEN - 1);
        s_state[i].name[PHI_GAMEPAD_NAME_LEN - 1] = 0;

        memset(s_state[i].buttons, 0, sizeof(s_state[i].buttons));
        int nb = ev.numButtons;
        if (nb > 16) nb = 16;
        for (int b = 0; b < nb; b++) {
            int mapped = STANDARD_BUTTON_MAP[b];
            if (mapped >= 0) s_state[i].buttons[mapped] = ev.digitalButton[b] ? 1 : 0;
        }

        for (int a = 0; a < PHI_GAMEPAD_AXIS_COUNT; a++) s_state[i].axes[a] = 0.0f;
        if (ev.numAxes >= 4) {
            s_state[i].axes[PHI_GAMEPAD_AXIS_LEFTX]  = (float)ev.axis[0];
            s_state[i].axes[PHI_GAMEPAD_AXIS_LEFTY]  = (float)ev.axis[1];
            s_state[i].axes[PHI_GAMEPAD_AXIS_RIGHTX] = (float)ev.axis[2];
            s_state[i].axes[PHI_GAMEPAD_AXIS_RIGHTY] = (float)ev.axis[3];
        }
        if (nb > 7) {
            s_state[i].axes[PHI_GAMEPAD_AXIS_LEFT_TRIGGER]  = (float)ev.analogButton[6];
            s_state[i].axes[PHI_GAMEPAD_AXIS_RIGHT_TRIGGER] = (float)ev.analogButton[7];
        }
    }
}

int phi_gamepad_count(void) {
    int n = 0;
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) if (s_state[i].connected) n++;
    return n;
}

const PhiGamepadState *phi_gamepad_get_state(int index) {
    if (index < 0 || index >= PHI_GAMEPAD_MAX) return NULL;
    return &s_state[index];
}
