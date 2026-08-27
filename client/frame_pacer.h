#pragma once

/* Real CPU-load capping -- see phi_platform.h's own comment on phi_
 * platform_sleep for why native and wasm need genuinely different
 * mechanisms here, not just different implementations of the same idea.
 *
 * Before this, neither native (Linux or Windows) nor wasm had ANY engine-
 * level frame pacing: native's main loop (phi_platform_native.c/_win32.c)
 * is a bare for(;;) with no sleep, no vsync control of its own, and no
 * cap of any kind -- confirmed by direct inspection, not assumed. The
 * existing `dt > 0.05f` clamp in editor_main.c's main_loop/player_main.c's
 * player_loop is a PHYSICS/ANIMATION STABILITY clamp only (bounds the
 * step size Bullet/skinning see after a slow frame, preventing a spiral
 * of death) -- it does nothing to limit actual CPU usage, a real and
 * easy distinction to miss. Wasm gets soft pacing for free via
 * emscripten_set_main_loop's requestAnimationFrame driving (typically
 * synced to the display's real refresh rate), but that's a courtesy of
 * the browser, not something the engine controls or can rely on as a
 * guaranteed cap (a backgrounded tab or unusual embedder can call rAF
 * faster, or without real vsync throttling).
 *
 * PHI_TARGET_CPU_FRACTION: this module keeps the calling thread's own
 * busy duty-cycle (fraction of each frame's wall-clock span actually
 * spent working, vs. idle/sleeping) at or below this fraction. 0.70 means
 * "never more than 70% busy" -- if a frame's real work takes T seconds,
 * the frame is stretched to at least T/0.70 seconds total by adding
 * T*(1/0.70 - 1) of idle time, so T / (T + idle) == 0.70 exactly.
 *
 * That T is a LOAD AVERAGE (frame_pacer.c's own s_avg_work_seconds), not
 * one frame's raw sample -- an exponentially-smoothed running average of
 * recent work time, maintained entirely inside this module's own state on
 * every platform, the same idea as Unix's load average. This is what
 * makes the whole module self-correcting with no external input, ever:
 * nobody needs to read a log line, profile a session, or hand anything
 * back in for the target to keep tracking real sustained load (a single
 * noisy frame doesn't itself yank the sleep target around either way).
 * See frame_pacer.c's own top comment for why this used to be wasm-only.
 *
 * One shared implementation (this file), not three hand-rolled ones --
 * added to ENGINE_CORE_SRCS so the same frame_pacer_begin/_end pair
 * reaches editor_main.c, player_main.c, and (transitively, since wasm
 * links the same ENGINE_CORE_SRCS) the browser build, matching this
 * project's own de-duplication pass this same session. */

#define PHI_TARGET_CPU_FRACTION 0.70f

/* Call once at the very start of each frame's real work (physics step,
 * skinned-object update, render submission -- the same span editor_
 * main.c's/player_main.c's existing dt clamp already covers). */
void frame_pacer_begin(void);

/* Call once per frame, right before phi_platform_swap() (present/vsync
 * wait) -- OPTIONAL, but strongly recommended, and what makes this
 * module self-correcting instead of silently wrong under vsync.
 *
 * Native SwapBuffers/glXSwapBuffers can genuinely BLOCK the calling
 * thread until the next vblank when vsync is on (this codebase never
 * calls wglSwapIntervalEXT/glXSwapIntervalEXT, so vsync is left at
 * whatever the driver defaults to -- commonly on). A blocked wait is
 * NOT cpu busy time -- the OS doesn't charge it against the process's
 * real CPU%, but before this function existed, frame_pacer_end() had no
 * way to tell it apart from real work and counted the whole span as
 * "busy", then added MORE sleep on top of an already-idle, vsync-
 * throttled frame -- throttling FPS/latency for no real CPU-saving
 * reason, the exact opposite of what PHI_TARGET_CPU_FRACTION promises.
 *
 * Calling this splits the frame_pacer_begin()..frame_pacer_end() span
 * into a real "work" portion (begin -> this call) that the idle-sleep
 * target is actually computed from, and a "present" portion (this call
 * -> end, i.e. however long the swap call itself took) that's measured
 * and exposed via frame_pacer_get_stats() below but deliberately excluded
 * from the duty-cycle math, since it was never real CPU work in the first
 * place. A caller that never calls this for a given frame just gets the
 * old, imprecise-under-vsync fallback (frame_pacer_end() treats the whole
 * span as work) -- safe, just not self-correcting for that one frame. */
void frame_pacer_mark_present(void);

/* Call once at the very end of each frame, after the frame is fully
 * submitted (present/swap included) -- computes this frame's real
 * CPU-work time (see frame_pacer_mark_present() above for how that's
 * separated from any present/vsync-wait time) and enforces PHI_TARGET_
 * CPU_FRACTION against THAT, not the whole frame span:
 *   - Native (Linux/Win32): calls phi_platform_sleep for exactly enough
 *     idle time (computed off the load average above, not this one
 *     frame's raw sample) to hit the target ratio -- a real, direct,
 *     verifiable effect (checkable via `ps`/`top` CPU%).
 *   - Wasm: cannot block-sleep the browser's main thread (would freeze
 *     the tab) -- instead calls emscripten_set_main_loop_timing(EM_
 *     TIMING_SETTIMEOUT, interval_ms), directly throttling how often the
 *     main-loop callback itself fires, with interval_ms recomputed from
 *     the same load average (this used to be the one platform that had
 *     smoothing at all -- see frame_pacer.c's own top comment). phi_
 *     platform_wasm.c's own phi_platform_swap is a true no-op (the
 *     browser presents on its own once the rAF callback returns), so
 *     this path was never affected by the vsync-conflation problem above
 *     -- real browser CPU% still isn't something this can be verified to
 *     hit without an actual browser profiler, and that limit is stated
 *     plainly rather than claimed as verified.
 *
 * Records this frame's own work/present/idle split for frame_pacer_get_
 * stats() below to hand back out -- see that function's own comment. */
void frame_pacer_end(void);

/* Read-only snapshot of what this module itself is measuring/doing --
 * the load average driving the sleep target (avg_work_seconds, see this
 * header's own PHI_TARGET_CPU_FRACTION comment) plus the most recent
 * single frame's own real work/present/idle split (seconds). This is the
 * public query surface for that data (also reachable via phi.h, so a
 * game/src/ *.c author can read it too) -- frame_pacer.c itself never
 * prints or logs any of this on its own; use this to build a debug HUD,
 * a console command, a game's own perf overlay, or just to inspect it
 * from a debugger, whatever the caller actually needs it for. Safe to
 * call any time; reads all-zero before the first frame_pacer_end() call
 * (a real, honest "nothing measured yet" state, not garbage). */
typedef struct {
    double avg_work_seconds;      /* the load average itself -- what frame_pacer_end() actually sleeps against on native */
    double last_work_seconds;     /* most recent frame's own real work time (present/vsync-wait excluded) */
    double last_present_seconds;  /* most recent frame's own present/vsync-wait time (0.0 on wasm, see phi_platform_wasm.c's phi_platform_swap) */
    double last_idle_seconds;     /* most recent frame's own actual slept idle time (0.0 on wasm -- interval-throttling has no directly observable "slept" duration) */
} FramePacerStats;

void frame_pacer_get_stats(FramePacerStats *out);
