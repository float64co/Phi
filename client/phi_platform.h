#pragma once
/* Platform abstraction layer (Phi Phase 0).
 *
 * Two implementations: phi_platform_wasm.c (Emscripten/WebGL1, matches the
 * existing Qek client exactly) and phi_platform_native.c (Xlib/GLX, OpenGL
 * 3.3 core). Everything outside these two files is plain C with zero
 * #ifdef __EMSCRIPTEN__ guards for windowing/context/timing concerns —
 * those all live behind this header now. Networking (phi_net.h) and input
 * event delivery are separate abstractions; this header only covers
 * window/context/timing/frame-loop.
 */

typedef struct {
    const char *title;    /* native window title; ignored by the wasm backend */
    int         width;    /* initial size hint; the wasm backend overrides this
                            * with the actual browser window size immediately */
    int         height;
} PhiPlatformConfig;

typedef void (*PhiMainLoopFn)(void *userdata);

/* Creates the window (native) or canvas + GL context (both). Must run before
 * any GL call or renderer_create(). */
void phi_platform_init(const PhiPlatformConfig *cfg);

/* Hands control to the platform's frame loop. Calls fn(userdata) once per
 * frame. Does not return on either backend: the wasm backend unwinds main()
 * via emscripten_set_main_loop's internal exception, the native backend
 * blocks in a for(;;) until phi_platform_should_close() would return true. */
void phi_platform_set_main_loop(PhiMainLoopFn fn, void *userdata);

/* Presents the frame. No-op on wasm (the browser swaps automatically after
 * the rAF callback returns); calls glXSwapBuffers on native. Present for
 * symmetry and so callers don't need to care which backend they're on. */
void phi_platform_swap(void);

/* Current drawable size in pixels. Cheap to call every frame — both
 * backends cache this and update it from a resize event, not a live query. */
void phi_platform_get_window_size(int *w, int *h);

/* Seconds, monotonic, arbitrary epoch — only deltas are meaningful. */
double phi_platform_now(void);

/* Native: true once the window close button / WM close request fires.
 * Wasm: always false (browser tabs don't get closed by the app). */
int phi_platform_should_close(void);

/* Fetches a GL function pointer by name. Native: wraps glXGetProcAddressARB,
 * used for every GL 2.0+/3.0+ entry point since <GL/gl.h> only declares up
 * to GL 1.2 on Linux. Wasm: unused — GLES2 entry points are already directly
 * linked against libGL via emcc — returns NULL. Deliberately hand-rolled
 * instead of pulling in GLEW/GLAD (see phi.md's "No GL loader library"). */
void *phi_gl_get_proc(const char *name);

void phi_platform_shutdown(void);
