/* Standalone, no-GL test harness for render_hooks.c's callback registry
 * -- proves register/invoke/unregister are really correct (invocation
 * order, per-point isolation, the userdata pass-through, exact-match
 * unregister, and the real PHI_RENDER_HOOKS_MAX_PER_POINT bound) without
 * needing a real GL context: render_hooks.c never dereferences the
 * GBuffer* itself, only passes it through to callbacks, so a dummy
 * pointer is a real, honest stand-in here, not a weakened test. */
#include "render_hooks.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

static char g_call_log[256];
static void log_call(const char *tag) {
    strncat(g_call_log, tag, sizeof(g_call_log) - strlen(g_call_log) - 1);
}

static void hook_a(GBuffer *gbuf, void *userdata) { (void)gbuf; (void)userdata; log_call("A"); }
static void hook_b(GBuffer *gbuf, void *userdata) { (void)gbuf; (void)userdata; log_call("B"); }
static void hook_c(GBuffer *gbuf, void *userdata) { (void)gbuf; (void)userdata; log_call("C"); }

static int g_last_userdata_seen = -1;
static void hook_records_userdata(GBuffer *gbuf, void *userdata) {
    (void)gbuf;
    g_last_userdata_seen = *(int *)userdata;
}

int main(void) {
    GBuffer dummy_gbuf;   /* never dereferenced by render_hooks.c itself -- see file comment */
    memset(&dummy_gbuf, 0, sizeof(dummy_gbuf));

    printf("[render_hooks_test] === 1: registration + invocation order ===\n");
    render_hooks_init();
    g_call_log[0] = 0;
    render_hooks_register(PHI_HOOK_AFTER_GBUFFER, hook_a, NULL);
    render_hooks_register(PHI_HOOK_AFTER_GBUFFER, hook_b, NULL);
    render_hooks_register(PHI_HOOK_AFTER_GBUFFER, hook_c, NULL);
    render_hooks_invoke(PHI_HOOK_AFTER_GBUFFER, &dummy_gbuf);
    CHECK(strcmp(g_call_log, "ABC") == 0, "three real hooks at the same point fire in real registration order (A, then B, then C)");

    printf("[render_hooks_test] === 2: points are really isolated from each other ===\n");
    g_call_log[0] = 0;
    render_hooks_invoke(PHI_HOOK_AFTER_LIGHTING, &dummy_gbuf);
    CHECK(g_call_log[0] == 0, "invoking a DIFFERENT point (after_lighting) doesn't fire hooks registered at after_gbuffer");
    render_hooks_invoke(PHI_HOOK_AFTER_RESOLVE, &dummy_gbuf);
    render_hooks_invoke(PHI_HOOK_AFTER_TONEMAP, &dummy_gbuf);
    CHECK(g_call_log[0] == 0, "the other two points are isolated too -- still nothing fired");

    printf("[render_hooks_test] === 3: userdata really round-trips to the callback ===\n");
    render_hooks_init();
    int payload = 42;
    render_hooks_register(PHI_HOOK_AFTER_TONEMAP, hook_records_userdata, &payload);
    render_hooks_invoke(PHI_HOOK_AFTER_TONEMAP, &dummy_gbuf);
    CHECK(g_last_userdata_seen == 42, "the exact userdata pointer passed to register() reached the callback, dereferenced correctly");

    printf("[render_hooks_test] === 4: unregister removes exactly the right (fn, userdata) pair ===\n");
    render_hooks_init();
    int ud1 = 1, ud2 = 2;
    render_hooks_register(PHI_HOOK_AFTER_GBUFFER, hook_records_userdata, &ud1);
    render_hooks_register(PHI_HOOK_AFTER_GBUFFER, hook_records_userdata, &ud2);
    int removed = render_hooks_unregister(PHI_HOOK_AFTER_GBUFFER, hook_records_userdata, &ud1);
    CHECK(removed == 1, "unregister on a real registered (fn, userdata) pair reports success");
    g_last_userdata_seen = -1;
    render_hooks_invoke(PHI_HOOK_AFTER_GBUFFER, &dummy_gbuf);
    CHECK(g_last_userdata_seen == 2, "only the ud2 registration survived -- ud1's specific registration was really removed, not just any hook_records_userdata entry");
    int removed_again = render_hooks_unregister(PHI_HOOK_AFTER_GBUFFER, hook_records_userdata, &ud1);
    CHECK(removed_again == 0, "unregistering an already-removed (fn, userdata) pair reports failure, not a false success");

    printf("[render_hooks_test] === 5: the per-point capacity bound is real ===\n");
    render_hooks_init();
    int registered = 0;
    for (int i = 0; i < PHI_RENDER_HOOKS_MAX_PER_POINT + 4; i++) {
        if (render_hooks_register(PHI_HOOK_AFTER_LIGHTING, hook_a, NULL)) registered++;
    }
    CHECK(registered == PHI_RENDER_HOOKS_MAX_PER_POINT, "exactly PHI_RENDER_HOOKS_MAX_PER_POINT registrations succeed at one point, further ones fail rather than overflow");

    printf("[render_hooks_test] === 6: invoking an unregistered/empty point is a safe no-op ===\n");
    render_hooks_init();
    render_hooks_invoke(PHI_HOOK_AFTER_RESOLVE, &dummy_gbuf);   /* would crash/UB if this weren't a real, safe no-op */
    CHECK(1, "invoking a point with zero registered hooks didn't crash");

    printf("\n[render_hooks_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
