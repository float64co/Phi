#include "frame_pacer.h"
#include "phi_platform.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

static double s_frame_start = 0.0;

/* Set by frame_pacer_mark_present() -- see frame_pacer.h's own comment on
 * why this split exists. s_have_present_mark resets to 0 every
 * frame_pacer_begin() call, so a frame that never marks (either the
 * caller doesn't call it, or an early return skips the swap entirely --
 * see e.g. player_main.c's player_render, which can return before ever
 * reaching phi_platform_swap()) safely falls back to the old whole-span-
 * is-work behavior in frame_pacer_end() below, rather than reading a
 * stale mark left over from a previous frame. */
static double s_present_mark = 0.0;
static int    s_have_present_mark = 0;

/* Load average -- the actual self-correcting mechanism (see frame_pacer_
 * end's own comment on why this replaced computing the sleep target
 * straight off one frame's raw `work` sample). An exponential moving
 * average of recent frame WORK time (seconds, present/vsync-wait already
 * excluded -- see frame_pacer_mark_present), maintained entirely inside
 * this module's own state on EVERY platform (this used to be wasm-only,
 * gated behind #ifdef __EMSCRIPTEN__, purely because that was the one
 * path that already needed smoothing for emscripten_set_main_loop_
 * timing's interval; native's sleep-target math used a single frame's
 * raw sample directly, with no smoothing at all, until now). Same idea
 * as Unix's load average: converges toward whatever the real recent load
 * is, continuously, for the life of the process, with no external input
 * of any kind -- nobody needs to read a log line, profile a session, or
 * feed anything back in for this to keep self-correcting. FRAME_PACER_
 * LOAD_AVG_ALPHA's 0.1 weight on each new sample is a real, if untuned,
 * starting point (fast enough to track a real sustained load change
 * within ~10-20 frames, slow enough that one unusually slow/fast frame
 * -- a GC pause, a one-off stutter, a frame that happened to trigger a
 * texture upload -- doesn't itself yank the target around). */
#define FRAME_PACER_LOAD_AVG_ALPHA 0.1
static double s_avg_work_seconds = 0.0;
static int    s_have_avg = 0;

/* Most recent frame's own measured split -- real numbers, not derived or
 * re-estimated, just the same `work`/`present`/`idle` values frame_pacer_
 * end() itself computes and acts on every frame, kept around so frame_
 * pacer_get_stats() (frame_pacer.h -- phi.h's public query surface for
 * this data) can hand them to a caller without frame_pacer.c needing to
 * print anything on its own. */
static double s_last_work_seconds    = 0.0;
static double s_last_present_seconds = 0.0;
static double s_last_idle_seconds    = 0.0;

void frame_pacer_begin(void) {
    s_frame_start = phi_platform_now();
    s_have_present_mark = 0;
}

void frame_pacer_mark_present(void) {
    s_present_mark = phi_platform_now();
    s_have_present_mark = 1;
}

void frame_pacer_get_stats(FramePacerStats *out) {
    out->avg_work_seconds     = s_avg_work_seconds;
    out->last_work_seconds    = s_last_work_seconds;
    out->last_present_seconds = s_last_present_seconds;
    out->last_idle_seconds    = s_last_idle_seconds;
}

void frame_pacer_end(void) {
    double now = phi_platform_now();
    /* No mark this frame -> present_mark collapses to `now`, so work ==
     * the whole span and present == 0 -- the same behavior this module
     * had before frame_pacer_mark_present existed, not a crash or a
     * divide-by-garbage. */
    double present_mark = s_have_present_mark ? s_present_mark : now;

    double work = present_mark - s_frame_start;
    if (work < 0.0) work = 0.0;   /* defensive: a clock that isn't strictly monotonic would otherwise go negative below */
    double present = now - present_mark;
    if (present < 0.0) present = 0.0;

    /* Feed this frame's real work sample into the load average -- see its
     * own comment above. Every platform updates it the same way; only
     * what each platform DOES with the resulting smoothed value differs
     * below. */
    if (!s_have_avg) { s_avg_work_seconds = work; s_have_avg = 1; }
    else s_avg_work_seconds = s_avg_work_seconds * (1.0 - FRAME_PACER_LOAD_AVG_ALPHA)
                             + work * FRAME_PACER_LOAD_AVG_ALPHA;

#ifdef __EMSCRIPTEN__
    double interval_ms = (s_avg_work_seconds * 1000.0) / (double)PHI_TARGET_CPU_FRACTION;
    if (interval_ms < 1.0) interval_ms = 1.0;   /* never request a 0/negative interval -- that's an uncapped busy-loop, exactly what this exists to prevent */
    emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, (int)interval_ms);
    s_last_work_seconds = work;
    s_last_present_seconds = present;
    s_last_idle_seconds = 0.0;   /* interval-throttling has no directly observable "slept" duration the way phi_platform_sleep's return does */
#else
    /* s_avg_work_seconds is the smoothed load average (present/vsync-wait
     * time already excluded from every sample that fed it, see frame_
     * pacer_mark_present); want work/(work+idle) == PHI_TARGET_CPU_
     * FRACTION, so idle == avg*(1/target - 1). Driving this off the
     * SMOOTHED average rather than this one frame's raw `work` value
     * (the previous approach) is the actual point of the load-average
     * change: a single noisy sample no longer directly dictates how long
     * this frame sleeps, the running average does, so the target tracks
     * genuinely sustained load instead of chasing every frame-to-frame
     * blip. */
    double idle = s_avg_work_seconds * (1.0 / (double)PHI_TARGET_CPU_FRACTION - 1.0);
    if (idle > 0.0) phi_platform_sleep(idle);
    else idle = 0.0;
    s_last_work_seconds = work;
    s_last_present_seconds = present;
    s_last_idle_seconds = idle;
#endif
}
