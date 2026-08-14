/* Standalone MicroPython DNA/RNA + @phi.panel test -- exercises
 * mp_port.c's phi.prop_get/phi.prop_set (backed by the real phi_prop.c
 * registry, phi_prop_registry.c's actual MeshObject/HEFace groups) and
 * the @phi.panel decorator wiring (phi_mp_panel_count/name/draw_panel)
 * against a real embedded interpreter -- same "prove it in isolation, no
 * GL/window needed" precedent as mesh_edit_test_main.c/
 * area_tree_test_main.c, applied to the new DNA/RNA binding layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp_port.h"
#include "meshobject.h"
#include "halfedge.h"
#include "halfedge_gltf.h"
#include "scene_target.h"
#include "render_settings.h"

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    int stack_top;

    printf("[mp_prop_panel_test] === setup: load a real MeshObject, register it as phi's 'object' target ===\n");
    MeshObject obj = {0};
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf");
    obj.id = 1;
    obj.position = (Vec3f){1.0f, 2.0f, 3.0f};
    obj.scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj.is_static = 1;
    obj.hem = hem;
    int loaded = 1;
    int edit_face = 0;
    RenderSettings rs = {128};

    phi_mp_init(&stack_top);
    /* This test doesn't exercise phi.enable_physics/apply_impulse/etc.
     * (see mp_physics_test_main.c for that) -- NULL is safe here since
     * mp_port.c only ever reads phys_world from inside those functions,
     * never during init/bootstrap. */
    phi_mp_register_targets(&obj, &loaded, &edit_face, NULL);
    /* phi.prop_get/set now resolve through scene_target.c's shared
     * resolver (see its own comment), not mp_port.c's old private one --
     * needs its own registration call too, same pointers. */
    scene_target_register(&obj, &loaded, &edit_face, &rs);

    printf("[mp_prop_panel_test] === 1: phi.prop_get('object', 'position') reads the REAL C struct via Python ===\n");
    char *out = phi_mp_exec("print(phi.prop_get('object', 'position'))");
    printf("  captured: %s", out);
    check(strstr(out, "1.0") != NULL && strstr(out, "2.0") != NULL && strstr(out, "3.0") != NULL,
          "the tuple Python sees matches the real C-side position (1,2,3)");
    free(out);

    printf("[mp_prop_panel_test] === 2: phi.prop_set('object', 'position', ...) writes the REAL C struct FROM Python ===\n");
    out = phi_mp_exec("phi.prop_set('object', 'position', (10.0, 20.0, 30.0))");
    free(out);
    check(obj.position.x == 10.0f && obj.position.y == 20.0f && obj.position.z == 30.0f,
          "the C-side MeshObject.position field actually changed after a Python call, not a copy");

    printf("[mp_prop_panel_test] === 3: phi.prop_get/set('object', 'is_static') -- BOOL through the same float-ish path ===\n");
    out = phi_mp_exec("print(phi.prop_get('object', 'is_static'))"); printf("  captured: %s", out); free(out);
    out = phi_mp_exec("phi.prop_set('object', 'is_static', 0.0)"); free(out);
    check(obj.is_static == 0, "set_static(0.0) cleared the real int field via Python");

    printf("[mp_prop_panel_test] === 4: phi.prop_get('face', 'metallic') and range clamping via phi.prop_set ===\n");
    out = phi_mp_exec("phi.prop_set('face', 'metallic', 5.0)");   /* out of [0,1] range */
    free(out);
    check(hem->faces[0].metallic == 1.0f, "range clamp (0..1) applies even when set from Python, not just from C callers");

    printf("[mp_prop_panel_test] === 5: unknown identifier raises a real, catchable Python exception ===\n");
    out = phi_mp_exec("phi.prop_get('object', 'not_a_real_property')");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL || strlen(out) > 0,
          "raises rather than crashing the process (still alive -- this check running proves it)");
    free(out);

    printf("[mp_prop_panel_test] === 6: target unavailable ('face' with no selection) raises cleanly ===\n");
    edit_face = -1;
    out = phi_mp_exec("phi.prop_get('face', 'metallic')");
    printf("  captured: %s", out);
    check(strlen(out) > 0, "raises when the target isn't currently resolvable, doesn't silently return garbage");
    free(out);
    edit_face = 0;

    printf("[mp_prop_panel_test] === 7: @phi.panel registers, C sees it immediately, draw(ctx) round-trips through real Python ===\n");
    out = phi_mp_exec(
        "@phi.panel('Test Panel')\n"
        "class TestPanel(phi.Panel):\n"
        "    def __init__(self):\n"
        "        self.n = 0\n"
        "    def draw(self, ctx):\n"
        "        self.n += 1\n"
        "        pos = ctx.prop_get('object', 'position')\n"
        "        return ['calls=%d' % self.n, 'x=%.1f' % pos[0]]\n"
    );
    printf("  captured: %s", out);
    free(out);
    check(phi_mp_panel_count() == 1, "exactly one panel registered");
    check(strcmp(phi_mp_panel_name(0), "Test Panel") == 0, "C sees the exact name passed to @phi.panel(...)");

    char lines[8][256];
    int n = phi_mp_draw_panel(0, lines, 8);
    printf("  draw_panel -> n=%d\n", n);
    for (int i = 0; i < n; i++) printf("    line[%d] = \"%s\"\n", i, lines[i]);
    check(n == 2, "draw() returned exactly the 2-item list it built");
    check(n == 2 && strcmp(lines[0], "calls=1") == 0, "first call -> calls=1 (real instance state, not re-initialized)");
    check(n == 2 && strncmp(lines[1], "x=10.0", 6) == 0, "second returned line reflects the REAL current C-side x (10.0, set in step 2)");

    n = phi_mp_draw_panel(0, lines, 8);
    check(n == 2 && strcmp(lines[0], "calls=2") == 0, "second draw_panel call -> calls=2 -- self.n truly persisted across calls, not reset");

    printf("[mp_prop_panel_test] === 8: a panel whose draw() raises reports failure, doesn't crash ===\n");
    out = phi_mp_exec(
        "@phi.panel('Bad Panel')\n"
        "class BadPanel(phi.Panel):\n"
        "    def draw(self, ctx):\n"
        "        raise RuntimeError('deliberate')\n"
    );
    free(out);
    check(phi_mp_panel_count() == 2, "second panel also registered despite the first one's draw() being broken");
    int bad_n = phi_mp_draw_panel(1, lines, 8);
    check(bad_n == -1, "draw_panel returns -1 when draw() raises");
    const char *last = phi_mp_last_captured_output();
    printf("  last captured output after the raising draw(): %.100s\n", last);
    check(strlen(last) > 0, "the traceback was captured, not silently swallowed");

    if (g_fail) printf("\n[mp_prop_panel_test] RESULT: FAIL\n");
    else printf("\n[mp_prop_panel_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
