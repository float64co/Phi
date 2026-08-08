#include "phi_platform.h"

#include <emscripten.h>
#include <emscripten/html5.h>

/* Everything here is a direct extraction of what main.c used to do inline
 * under #ifdef __EMSCRIPTEN__ — pure refactor, no behavior change. */

static PhiMainLoopFn s_loop_fn = NULL;
static void         *s_loop_ud = NULL;
static int           s_w = 0, s_h = 0;

static void resize_canvas_to_window(void) {
    int w = EM_ASM_INT({ return window.innerWidth;  });
    int h = EM_ASM_INT({ return window.innerHeight; });
    EM_ASM({
        var c = document.getElementById('canvas');
        c.width  = $0;
        c.height = $1;
    }, w, h);
    s_w = w;
    s_h = h;
}

static EM_BOOL on_window_resize(int type, const EmscriptenUiEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    resize_canvas_to_window();
    return EM_TRUE;
}

static void trampoline(void) {
    s_loop_fn(s_loop_ud);
}

void phi_platform_init(const PhiPlatformConfig *cfg) {
    (void)cfg;  /* wasm always fills the browser window; width/height hints are unused */

    resize_canvas_to_window();

    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 1;
    attr.minorVersion = 0;
    attr.antialias    = 1;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx =
        emscripten_webgl_create_context("#canvas", &attr);
    emscripten_webgl_make_context_current(ctx);

    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_window_resize);
}

void phi_platform_set_main_loop(PhiMainLoopFn fn, void *userdata) {
    s_loop_fn = fn;
    s_loop_ud = userdata;
    emscripten_set_main_loop(trampoline, 0, 1);
}

void phi_platform_swap(void) {
    /* no-op: the browser presents automatically once the rAF callback returns */
}

void phi_platform_get_window_size(int *w, int *h) {
    *w = s_w;
    *h = s_h;
}

double phi_platform_now(void) {
    return emscripten_get_now() / 1000.0;
}

int phi_platform_should_close(void) {
    return 0;  /* browser tabs aren't closed by the app */
}

void *phi_gl_get_proc(const char *name) {
    (void)name;
    return NULL;  /* unused on wasm: GLES2 entry points are directly linked via emcc */
}

void phi_platform_shutdown(void) {
    /* no-op: page teardown handles everything */
}
