#pragma once

/* Gamepad/Steam Deck input -- Phase 9 (see phi.md's Hard Architectural
 * Decisions: "No SDL/GLFW for windowing, GL context, or audio", with a
 * deliberate, narrow exception for gamepad input specifically). Three
 * real backends implement this same interface:
 *   input_gamepad_native.c  -- Linux, via vendored SDL2's GameController
 *                               subsystem (client/vendor/SDL2, joystick+
 *                               gamecontroller only -- see its own
 *                               VENDORED.md for what's in there and why).
 *   input_gamepad_win32_stub.c -- Windows: a real, honest stub for now
 *                               (see its own file comment for why).
 *   input_gamepad_wasm.c    -- Emscripten's own native Gamepad API
 *                               bindings, backed directly by the
 *                               browser's standard Gamepad API. No SDL2
 *                               on this target at all -- Emscripten
 *                               doesn't need it as an intermediary.
 *
 * PhiGamepadButton/PhiGamepadAxis are Phi's own enums (numerically
 * independent of SDL2's), so nothing outside input_gamepad_native.c ever
 * needs to see an SDL2 type -- matching this codebase's other "thin API
 * over a real embedded subsystem" headers (phi_physics.h, mp_port.h).
 * Deliberately scoped to the classic/stable 15-button, 6-axis "standard
 * gamepad" layout (W3C Standard Gamepad Mapping / SDL2's original core
 * button set) -- NOT SDL2 2.24+'s newer misc1/paddle/touchpad buttons,
 * which aren't universally supported and would add real complexity
 * (especially on the wasm backend, which only gets what the browser's
 * own "standard" mapping reports) for unclear near-term value. A real,
 * stated MVP scope choice, not an oversight. */

#define PHI_GAMEPAD_MAX 4   /* simultaneous gamepads -- same "small fixed cap" convention as PHI_MAX_LIGHTS/SCENE_MAX_OBJECTS elsewhere in this codebase */
#define PHI_GAMEPAD_NAME_LEN 64

typedef enum {
    PHI_GAMEPAD_BUTTON_A = 0,
    PHI_GAMEPAD_BUTTON_B,
    PHI_GAMEPAD_BUTTON_X,
    PHI_GAMEPAD_BUTTON_Y,
    PHI_GAMEPAD_BUTTON_BACK,
    PHI_GAMEPAD_BUTTON_GUIDE,
    PHI_GAMEPAD_BUTTON_START,
    PHI_GAMEPAD_BUTTON_LEFTSTICK,
    PHI_GAMEPAD_BUTTON_RIGHTSTICK,
    PHI_GAMEPAD_BUTTON_LEFTSHOULDER,
    PHI_GAMEPAD_BUTTON_RIGHTSHOULDER,
    PHI_GAMEPAD_BUTTON_DPAD_UP,
    PHI_GAMEPAD_BUTTON_DPAD_DOWN,
    PHI_GAMEPAD_BUTTON_DPAD_LEFT,
    PHI_GAMEPAD_BUTTON_DPAD_RIGHT,
    PHI_GAMEPAD_BUTTON_COUNT,
} PhiGamepadButton;

typedef enum {
    PHI_GAMEPAD_AXIS_LEFTX = 0,
    PHI_GAMEPAD_AXIS_LEFTY,
    PHI_GAMEPAD_AXIS_RIGHTX,
    PHI_GAMEPAD_AXIS_RIGHTY,
    PHI_GAMEPAD_AXIS_LEFT_TRIGGER,
    PHI_GAMEPAD_AXIS_RIGHT_TRIGGER,
    PHI_GAMEPAD_AXIS_COUNT,
} PhiGamepadAxis;

typedef struct {
    int   connected;
    char  name[PHI_GAMEPAD_NAME_LEN];
    /* Sticks: -1..1 (0 centered). Triggers: 0..1 (0 released). Already
     * normalized/clamped by whichever backend filled this in -- callers
     * never see a raw device-specific range. */
    float axes[PHI_GAMEPAD_AXIS_COUNT];
    int   buttons[PHI_GAMEPAD_BUTTON_COUNT];   /* 0/1 */
} PhiGamepadState;

/* Call once at startup (after phi_platform_init, before the first
 * phi_gamepad_poll) -- same "registry init" convention as scene_objects_
 * init/render_hooks_init elsewhere in this codebase. */
void phi_gamepad_init(void);

/* Call once at shutdown. Safe to call even if phi_gamepad_init failed or
 * was never called. */
void phi_gamepad_shutdown(void);

/* Call once per frame -- refreshes every connected pad's real state
 * (button presses, stick/trigger positions), and detects newly
 * connected/disconnected devices. Cheap and safe to call unconditionally
 * even with zero gamepads attached. */
void phi_gamepad_poll(void);

/* How many of the PHI_GAMEPAD_MAX slots are currently connected. */
int phi_gamepad_count(void);

/* index in [0, PHI_GAMEPAD_MAX) -- NULL if out of range. A valid,
 * non-NULL return does NOT mean a real device is attached at that slot;
 * check ->connected (a disconnected slot's other fields are zeroed, not
 * stale/garbage, so reading them without checking ->connected first is
 * safe, just meaningless). */
const PhiGamepadState *phi_gamepad_get_state(int index);
