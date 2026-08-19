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

/* Call once at the very end of that same span, after the frame is fully
 * submitted (present/swap included) -- computes this frame's real
 * elapsed wall-clock work time and enforces PHI_TARGET_CPU_FRACTION:
 *   - Native (Linux/Win32): calls phi_platform_sleep for exactly enough
 *     idle time to hit the target ratio -- a real, direct, verifiable
 *     effect (checkable via `ps`/`top` CPU%).
 *   - Wasm: cannot block-sleep the browser's main thread (would freeze
 *     the tab) -- instead calls emscripten_set_main_loop_timing(EM_
 *     TIMING_SETTIMEOUT, interval_ms), directly throttling how often the
 *     main-loop callback itself fires, with interval_ms recomputed from
 *     a short exponential moving average of recent work time (smoothed
 *     so one slow frame doesn't cause visible stutter). This builds and
 *     calls the right API; real browser CPU% isn't something this can be
 *     verified to hit without an actual browser profiler, and that
 *     limit is stated plainly rather than claimed as verified. */
void frame_pacer_end(void);
