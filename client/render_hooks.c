#include "render_hooks.h"
#include <string.h>

typedef struct {
    PhiRenderHookFn fn;
    void *userdata;
} PhiHookSlot;

static PhiHookSlot s_hooks[PHI_HOOK_POINT_COUNT][PHI_RENDER_HOOKS_MAX_PER_POINT];
static int         s_hook_count[PHI_HOOK_POINT_COUNT];

void render_hooks_init(void) {
    memset(s_hooks, 0, sizeof(s_hooks));
    memset(s_hook_count, 0, sizeof(s_hook_count));
}

int render_hooks_register(PhiRenderHookPoint point, PhiRenderHookFn fn, void *userdata) {
    if (point < 0 || point >= PHI_HOOK_POINT_COUNT) return 0;
    if (s_hook_count[point] >= PHI_RENDER_HOOKS_MAX_PER_POINT) return 0;
    int slot = s_hook_count[point]++;
    s_hooks[point][slot].fn = fn;
    s_hooks[point][slot].userdata = userdata;
    return 1;
}

int render_hooks_unregister(PhiRenderHookPoint point, PhiRenderHookFn fn, void *userdata) {
    if (point < 0 || point >= PHI_HOOK_POINT_COUNT) return 0;
    for (int i = 0; i < s_hook_count[point]; i++) {
        if (s_hooks[point][i].fn == fn && s_hooks[point][i].userdata == userdata) {
            /* Swap the last slot into this one -- registration ORDER among
             * the remaining hooks after a removal isn't a guarantee this
             * API makes (see render_hooks_invoke's own comment: "in
             * registration order" describes the steady-state case, not
             * what survives a mid-sequence removal), so this is a real,
             * honest O(1) removal rather than an O(n) shift that would
             * only be needed to preserve an ordering guarantee this API
             * doesn't actually promise. */
            int last = --s_hook_count[point];
            s_hooks[point][i] = s_hooks[point][last];
            s_hooks[point][last].fn = NULL;
            s_hooks[point][last].userdata = NULL;
            return 1;
        }
    }
    return 0;
}

void render_hooks_invoke(PhiRenderHookPoint point, GBuffer *gbuf) {
    if (point < 0 || point >= PHI_HOOK_POINT_COUNT) return;
    for (int i = 0; i < s_hook_count[point]; i++) {
        s_hooks[point][i].fn(gbuf, s_hooks[point][i].userdata);
    }
}
