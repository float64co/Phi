#include "frame_pacer.h"
#include "phi_platform.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

static double s_frame_start = 0.0;

#ifdef __EMSCRIPTEN__
/* Exponential moving average of recent frame work time (seconds) --
 * smoothed so one unusually slow/fast frame doesn't yank the callback
 * interval around and cause visible stutter. 0.1 weight on each new
 * sample is a real, if untuned, starting point (fast enough to track a
 * real sustained load change within ~10-20 frames, slow enough to ignore
 * a one-off spike) -- see frame_pacer.h's own comment on why wasm can't
 * be verified against a real browser profiler here. */
static double s_avg_work_seconds = 0.0;
static int    s_have_avg = 0;
#endif

void frame_pacer_begin(void) {
    s_frame_start = phi_platform_now();
}

void frame_pacer_end(void) {
    double elapsed = phi_platform_now() - s_frame_start;
    if (elapsed < 0.0) elapsed = 0.0;   /* defensive: a clock that isn't strictly monotonic would otherwise go negative below */

#ifdef __EMSCRIPTEN__
    if (!s_have_avg) { s_avg_work_seconds = elapsed; s_have_avg = 1; }
    else s_avg_work_seconds = s_avg_work_seconds * 0.9 + elapsed * 0.1;

    double interval_ms = (s_avg_work_seconds * 1000.0) / (double)PHI_TARGET_CPU_FRACTION;
    if (interval_ms < 1.0) interval_ms = 1.0;   /* never request a 0/negative interval -- that's an uncapped busy-loop, exactly what this exists to prevent */
    emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, (int)interval_ms);
#else
    /* elapsed is this frame's busy duty-cycle; want elapsed/(elapsed+idle)
     * == PHI_TARGET_CPU_FRACTION, so idle == elapsed*(1/target - 1). */
    double idle = elapsed * (1.0 / (double)PHI_TARGET_CPU_FRACTION - 1.0);
    if (idle > 0.0) phi_platform_sleep(idle);
#endif
}
