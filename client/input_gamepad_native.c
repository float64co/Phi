/* Linux native backend for input_gamepad.h -- real SDL2 GameController
 * subsystem (client/vendor/SDL2, joystick+gamecontroller only, see its
 * own VENDORED.md). Windows uses input_gamepad_win32_stub.c instead (see
 * its own comment for why); wasm uses input_gamepad_wasm.c (no SDL2
 * involved there at all). */
#include "input_gamepad.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>

static SDL_GameController *s_pads[PHI_GAMEPAD_MAX];
static PhiGamepadState     s_state[PHI_GAMEPAD_MAX];
static int                 s_ready = 0;

void phi_gamepad_init(void) {
    memset(s_pads, 0, sizeof(s_pads));
    memset(s_state, 0, sizeof(s_state));
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        printf("[input_gamepad] SDL_InitSubSystem(GAMECONTROLLER) failed: %s -- gamepad input unavailable this session\n", SDL_GetError());
        s_ready = 0;
        return;
    }
    s_ready = 1;
    printf("[input_gamepad] real SDL2 GameController subsystem ready\n");
}

void phi_gamepad_shutdown(void) {
    if (!s_ready) return;
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
        if (s_pads[i]) SDL_GameControllerClose(s_pads[i]);
    }
    memset(s_pads, 0, sizeof(s_pads));
    memset(s_state, 0, sizeof(s_state));
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    s_ready = 0;
}

/* PhiGamepadButton/Axis index -> the matching real SDL_GameController*
 * enum value -- a plain lookup table, not a numeric coincidence (see
 * input_gamepad.h's own comment on why Phi's enums are numerically
 * independent of SDL2's). */
static const SDL_GameControllerButton BUTTON_MAP[PHI_GAMEPAD_BUTTON_COUNT] = {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
};
static const SDL_GameControllerAxis AXIS_MAP[PHI_GAMEPAD_AXIS_COUNT] = {
    SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
};

static int slot_for_instance(SDL_JoystickID id) {
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
        if (s_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == id) return i;
    }
    return -1;
}

void phi_gamepad_poll(void) {
    if (!s_ready) return;
    SDL_GameControllerUpdate();

    /* Close any that disconnected since the last poll. */
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
        if (s_pads[i] && !SDL_GameControllerGetAttached(s_pads[i])) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
            memset(&s_state[i], 0, sizeof(s_state[i]));
        }
    }

    /* Open newly connected controllers into the first free slot. */
    int n = SDL_NumJoysticks();
    for (int j = 0; j < n && j >= 0; j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        if (slot_for_instance(id) >= 0) continue;   /* already open */
        for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
            if (s_pads[i]) continue;
            s_pads[i] = SDL_GameControllerOpen(j);
            if (s_pads[i]) {
                s_state[i].connected = 1;
                const char *name = SDL_GameControllerName(s_pads[i]);
                strncpy(s_state[i].name, name ? name : "Unknown", PHI_GAMEPAD_NAME_LEN - 1);
                s_state[i].name[PHI_GAMEPAD_NAME_LEN - 1] = 0;
                printf("[input_gamepad] connected: %s\n", s_state[i].name);
            }
            break;
        }
    }

    /* Sample live state for every open pad. */
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
        if (!s_pads[i]) continue;
        for (int b = 0; b < PHI_GAMEPAD_BUTTON_COUNT; b++) {
            s_state[i].buttons[b] = SDL_GameControllerGetButton(s_pads[i], BUTTON_MAP[b]) ? 1 : 0;
        }
        for (int a = 0; a < PHI_GAMEPAD_AXIS_COUNT; a++) {
            Sint16 raw = SDL_GameControllerGetAxis(s_pads[i], AXIS_MAP[a]);
            float norm = raw / 32767.0f;
            if (a == PHI_GAMEPAD_AXIS_LEFT_TRIGGER || a == PHI_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                if (norm < 0.0f) norm = 0.0f;   /* triggers: SDL reports 0..32767, never negative in practice, clamp defensively */
            } else {
                if (norm < -1.0f) norm = -1.0f;   /* sticks: SDL's -32768 would normalize to -1.0000305, clamp to exactly -1 */
                if (norm > 1.0f) norm = 1.0f;
            }
            s_state[i].axes[a] = norm;
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
