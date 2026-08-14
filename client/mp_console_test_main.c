/* Standalone test harness for mp_port.c's Console-facing glue
 * (phi_mp_init/phi_mp_exec/phi_mp_capture_output) -- the piece Phase 1's
 * "Console panel becomes a real Python REPL" actually depends on, as
 * distinct from client/mp_test_main.c (which validates Phase 5's
 * @phi.panel/@phi.node decorator patterns, explicitly out of scope for
 * this pass) and client/mp_stress_test_main.c (synthetic load). Exists
 * because this session's X11 environment has been unreliable-to-fully-
 * unresponsive throughout (see phi.md), so the actual Console panel's
 * live keystroke-to-scrollback round trip can't be screenshot-verified --
 * but the interpreter plumbing underneath it has no GL/window dependency
 * at all and is fully checkable here, independent of the UI wired around
 * it in console.c. */
#include "mp_port.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

/* Stub -- see mp_test_main.c's own identical stub for why this is needed
 * (mp_port.c now links scene_objects.c, whose scene_object_delete
 * references mesh_destroy, octree_render.c's real GL-touching home). */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

int main(void) {
    int stack_top;
    phi_mp_init(&stack_top);

    printf("[mp_console_test] === 1: print() output is genuinely captured, not lost to stdout ===\n");
    char *out = phi_mp_exec("print('hello from python')");
    printf("[mp_console_test] captured: \"%s\"\n", out);
    CHECK(strstr(out, "hello from python") != NULL, "captured output contains the printed text");
    free(out);

    printf("[mp_console_test] === 2: real computation happens, not just echo ===\n");
    out = phi_mp_exec("x = 21 * 2\nprint(x)");
    printf("[mp_console_test] captured: \"%s\"\n", out);
    CHECK(strstr(out, "42") != NULL, "computed value 42 appears in captured output");
    free(out);

    printf("[mp_console_test] === 3: multi-line output captured with real line breaks ===\n");
    out = phi_mp_exec("for i in range(3):\n    print('line', i)");
    printf("[mp_console_test] captured: \"%s\"\n", out);
    int newline_count = 0;
    for (const char *p = out; *p; p++) if (*p == '\n') newline_count++;
    CHECK(newline_count >= 3, "at least 3 newlines for 3 separate print() calls");
    CHECK(strstr(out, "line 0") && strstr(out, "line 1") && strstr(out, "line 2"),
          "all three loop iterations' output present");
    free(out);

    printf("[mp_console_test] === 4: uncaught exception is captured as a traceback, not a crash ===\n");
    out = phi_mp_exec("raise ValueError('deliberate console test exception')");
    printf("[mp_console_test] captured: \"%s\"\n", out);
    CHECK(strstr(out, "ValueError") != NULL, "exception type name appears in the captured traceback");
    CHECK(strstr(out, "deliberate console test exception") != NULL, "exception message appears in the captured traceback");
    free(out);
    printf("[mp_console_test] (process still alive after the exception -- this line running proves it)\n");

    printf("[mp_console_test] === 5: capture buffer resets between calls, no cross-contamination ===\n");
    out = phi_mp_exec("print('first call marker')");
    free(out);
    out = phi_mp_exec("print('second call marker')");
    CHECK(strstr(out, "first call marker") == NULL, "a previous call's output does not leak into this one");
    CHECK(strstr(out, "second call marker") != NULL, "this call's own output is present");
    free(out);

    printf("[mp_console_test] === 6: globals persist across calls (real REPL state, not a fresh interpreter each time) ===\n");
    out = phi_mp_exec("persistent_var = 100");
    free(out);
    out = phi_mp_exec("print(persistent_var + 1)");
    printf("[mp_console_test] captured: \"%s\"\n", out);
    CHECK(strstr(out, "101") != NULL, "a variable set in one exec call is visible (and usable) in the next");
    free(out);

    if (g_fail) { printf("\n[mp_console_test] RESULT: FAIL\n"); return 1; }
    printf("\n[mp_console_test] RESULT: PASS (all checks passed)\n");
    return 0;
}
