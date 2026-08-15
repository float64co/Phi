/* Standalone test for input_gamepad.h's native backend (input_gamepad_
 * native.c) -- real vendored SDL2 (client/vendor/SDL2, joystick+
 * gamecontroller only, see its own VENDORED.md), not a mock. This
 * sandbox has no physical gamepad attached, so "0 gamepads connected" is
 * the correct, expected result throughout -- not a failure. Two halves:
 * (1) input_gamepad.h's own public interface, the thing Phi's main.c and
 * phi.h consumers actually use; (2) a few checks directly against the
 * vendored SDL2 library underneath it, specifically confirming the
 * built-in SDL_GameControllerDB mapping database is really linked in and
 * really works -- promised explicitly in VENDORED.md's own "Verification
 * performed" section, honored here rather than only ever having been
 * proven in a throwaway manual smoke test. */
#include "input_gamepad.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("[input_gamepad_test] === 1: phi_gamepad_init/poll/shutdown, real SDL2 backend, no device attached ===\n");
    phi_gamepad_init();
    check(1, "phi_gamepad_init() returned without crashing (see stdout above for the real 'GameController subsystem ready' log line)");

    for (int frame = 0; frame < 5; frame++) phi_gamepad_poll();
    check(1, "phi_gamepad_poll() survived 5 real calls without crashing (simulating 5 frames with nothing connected)");

    check(phi_gamepad_count() == 0, "phi_gamepad_count() reports 0 -- correct, this sandbox has no physical gamepad attached");

    printf("[input_gamepad_test] === 2: phi_gamepad_get_state -- real bounds checking, real zeroed-not-garbage state ===\n");
    int all_valid_and_disconnected = 1;
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) {
        const PhiGamepadState *s = phi_gamepad_get_state(i);
        if (!s || s->connected != 0) { all_valid_and_disconnected = 0; break; }
    }
    check(all_valid_and_disconnected, "every valid slot [0, PHI_GAMEPAD_MAX) returns a real, non-NULL, ->connected==0 state -- not NULL, not garbage");

    check(phi_gamepad_get_state(-1) == NULL, "phi_gamepad_get_state(-1) returns NULL -- real bounds checking, not undefined behavior");
    check(phi_gamepad_get_state(PHI_GAMEPAD_MAX) == NULL, "phi_gamepad_get_state(PHI_GAMEPAD_MAX) returns NULL -- the real off-by-one boundary is actually checked");
    check(phi_gamepad_get_state(999) == NULL, "phi_gamepad_get_state(999) (wildly out of range) returns NULL too");

    /* A disconnected slot's OTHER fields (name/axes/buttons) must be real
     * zeros, not stale/uninitialized memory -- checked directly since a
     * caller reading them without checking ->connected first (a real,
     * documented-as-safe use per input_gamepad.h's own comment) must see
     * zero, not garbage. */
    const PhiGamepadState *s0 = phi_gamepad_get_state(0);
    int zeroed = (s0->name[0] == 0);
    for (int a = 0; a < PHI_GAMEPAD_AXIS_COUNT && zeroed; a++) if (s0->axes[a] != 0.0f) zeroed = 0;
    for (int b = 0; b < PHI_GAMEPAD_BUTTON_COUNT && zeroed; b++) if (s0->buttons[b] != 0) zeroed = 0;
    check(zeroed, "a disconnected slot's name/axes/buttons are real zeros, not uninitialized/stale memory");

    printf("[input_gamepad_test] === 3: shutdown, then a real re-init, both survive cleanly ===\n");
    phi_gamepad_shutdown();
    check(phi_gamepad_count() == 0, "phi_gamepad_count() is still a safe 0 after shutdown, not a crash/use-after-free");
    phi_gamepad_init();
    phi_gamepad_poll();
    check(phi_gamepad_count() == 0, "a real re-init after shutdown works cleanly too (init isn't a one-shot-only call)");
    phi_gamepad_shutdown();

    printf("[input_gamepad_test] === 4: directly against the vendored SDL2 library underneath -- confirms SDL_GameControllerDB is REALLY linked in ===\n");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        printf("  FAIL: SDL_InitSubSystem(GAMECONTROLLER): %s\n", SDL_GetError());
        g_fail = 1;
    } else {
        check(1, "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) succeeded against the real vendored library");
        /* A real, well-known Xbox 360 controller GUID+mapping string --
         * if the built-in SDL_GameControllerdb.h (1000+ real entries)
         * is really compiled in, SDL2 already knows this exact mapping,
         * so adding it again returns 0 ("updated an existing mapping"),
         * not 1 ("added a brand new one"). This is the same real check
         * VENDORED.md's own "Verification performed" section describes. */
        int result = SDL_GameControllerAddMapping(
            "030000005e0400008e02000014010000,Xbox 360 Controller,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,"
            "leftshoulder:b4,rightshoulder:b5,leftstick:b9,rightstick:b10,leftx:a0,lefty:a1,rightx:a3,"
            "righty:a4,lefttrigger:a2,righttrigger:a5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,"
        );
        check(result == 0, "SDL_GameControllerAddMapping reports this real Xbox 360 GUID as ALREADY KNOWN (0=updated, not 1=newly added) -- proves the built-in SDL_GameControllerDB is really compiled in and really working, not silently empty");
        check(strcmp(SDL_GameControllerGetStringForButton(SDL_CONTROLLER_BUTTON_A), "a") == 0, "SDL_GameControllerGetStringForButton(A) returns the real string 'a'");
        check(SDL_CONTROLLER_BUTTON_MAX > 0 && SDL_CONTROLLER_AXIS_MAX > 0, "real, non-degenerate button/axis enum counts from the vendored headers");
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }

    printf("\n[input_gamepad_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
