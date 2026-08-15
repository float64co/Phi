/* Windows backend for input_gamepad.h -- a real, honest stub, not a
 * silently-broken implementation. This environment has no MinGW
 * toolchain to actually build or run a win32 target against (matching
 * every other win32 limitation already noted throughout this project's
 * history -- e.g. phi.md's own Phase 0 notes on how win32 rendering/
 * input/networking parity was each only claimed AFTER verification
 * against real Windows hardware, not attempted blind), so a real
 * SDL_config_windows.h + SDL2-for-Windows integration (SDL2 ships one,
 * hand-maintained upstream, which would be the natural next step) or a
 * direct XInput backend were both left for whoever next has a real
 * Windows environment to verify against, rather than shipped untested.
 *
 * This stub reports zero gamepads always -- a real, valid, non-crashing
 * answer (matches PhiGamepadState's own contract: an unconnected slot's
 * fields are zeroed, not garbage), not a build failure or a silent
 * wrong answer. Every function here still does something real (clears
 * state, bounds-checks), it just never has a device to report. */
#include "input_gamepad.h"
#include <string.h>
#include <stdio.h>

static PhiGamepadState s_state[PHI_GAMEPAD_MAX];
static int             s_warned = 0;

void phi_gamepad_init(void) {
    memset(s_state, 0, sizeof(s_state));
    if (!s_warned) {
        printf("[input_gamepad] win32: not implemented yet (no verified build environment for it this pass) -- reporting 0 gamepads\n");
        s_warned = 1;
    }
}

void phi_gamepad_shutdown(void) {
    memset(s_state, 0, sizeof(s_state));
}

void phi_gamepad_poll(void) {
    /* Deliberately a no-op -- every slot stays zeroed/disconnected. */
}

int phi_gamepad_count(void) {
    return 0;
}

const PhiGamepadState *phi_gamepad_get_state(int index) {
    if (index < 0 || index >= PHI_GAMEPAD_MAX) return NULL;
    return &s_state[index];
}
