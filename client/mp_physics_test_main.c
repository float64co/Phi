/* Standalone MicroPython + Bullet Physics integration test -- exercises
 * mp_port.c's phi.enable_physics/apply_impulse/get_velocity/set_velocity
 * (the Phase 2 Python API surface, see phi.md's "Bullet Physics via
 * Emscripten") against a REAL embedded interpreter driving REAL Bullet
 * simulation, not a mock of either. Same "prove it end to end, no GL/
 * window needed" precedent as mp_prop_panel_test_main.c, extended to
 * cover physics specifically. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp_port.h"
#include "meshobject.h"
#include "halfedge.h"
#include "halfedge_gltf.h"
#include "phi_physics.h"

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    int stack_top;

    printf("[mp_physics_test] === setup: real MeshObject + real physics world + ground, registered with mp_port ===\n");
    MeshObject obj = {0};
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf");
    for (int i = 0; i < hem->vert_count; i++)
        for (int a = 0; a < 3; a++)
            hem->verts[i].pos[a] *= 16.0f;   /* match main.c's own scale */
    obj.id = 1;
    obj.position = (Vec3f){128.0f, 100.0f, 90.0f};
    obj.orientation = quat_identity();
    obj.hem = hem;
    int loaded = 1;
    int edit_face = -1;

    PhiPhysicsWorld *world = phi_physics_world_create();
    float identity_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Vec3f ground_half = {200.0f, 10.0f, 200.0f};
    Vec3f ground_pos  = {128.0f, 40.0f, 90.0f};
    phi_physics_add_box_body(world, ground_half, ground_pos, identity_quat, 0.0f, 0.3f);

    phi_mp_init(&stack_top);
    phi_mp_register_targets(&obj, &loaded, &edit_face, world);

    printf("[mp_physics_test] === 1: physics calls before enable_physics raise cleanly, don't crash ===\n");
    char *out = phi_mp_exec("phi.apply_impulse((1,0,0), (0,0,0))");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "apply_impulse before enable_physics raises ValueError (process still alive, this check proves it)");
    free(out);
    out = phi_mp_exec("phi.get_velocity()");
    check(strstr(out, "ValueError") != NULL, "get_velocity before enable_physics also raises");
    free(out);

    printf("[mp_physics_test] === 2: phi.enable_physics(mass, restitution) creates a REAL body via Python ===\n");
    check(obj.phys_body == NULL, "no phys_body before the call");
    out = phi_mp_exec("phi.enable_physics(1.0, 0.3)");
    printf("  captured: %s", out);
    free(out);
    check(obj.phys_body != NULL, "obj.phys_body is now non-NULL -- Python really created a C-side Bullet body");

    out = phi_mp_exec("phi.enable_physics(1.0, 0.3)");
    printf("  captured (2nd call): %s", out);
    check(strstr(out, "ValueError") != NULL, "a second enable_physics call raises (already has a body) rather than leaking/replacing it");
    free(out);

    printf("[mp_physics_test] === 3: real gravity actually moves obj.position (stepped from C, exactly like main_loop) ===\n");
    float start_y = obj.position.y;
    for (int i = 0; i < 120; i++) {
        phi_physics_world_step(world, 1.0f / 60.0f);
        float o[4];
        phi_physics_get_transform(obj.phys_body, &obj.position, o);
    }
    printf("  y: %.2f -> %.2f after 2s of simulated falling\n", start_y, obj.position.y);
    check(obj.position.y < start_y - 5.0f, "the object created via Python physics actually falls under real gravity");

    printf("[mp_physics_test] === 4: phi.get_velocity/set_velocity round-trip through Python ===\n");
    out = phi_mp_exec("phi.set_velocity((0.0, 0.0, 0.0))");
    free(out);
    out = phi_mp_exec("print(phi.get_velocity())");
    printf("  captured: %s", out);
    check(strstr(out, "0.0") != NULL, "velocity forced to zero from Python reads back as zero");
    free(out);

    printf("[mp_physics_test] === 5: phi.apply_impulse gives real velocity, readable back from Python ===\n");
    out = phi_mp_exec("phi.apply_impulse((3.0, 0.0, 0.0), (0.0, 0.0, 0.0))\nprint(phi.get_velocity())");
    printf("  captured: %s", out);
    check(strstr(out, "3.0") != NULL, "an impulse applied from Python produces real velocity, read back correctly (impulse/mass = 3.0/1.0)");
    free(out);

    printf("[mp_physics_test] === 6: bad argument shapes raise instead of corrupting memory ===\n");
    out = phi_mp_exec("phi.apply_impulse((1,2), (0,0,0))");   /* 2-tuple, not 3 */
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "a malformed 2-element vector raises cleanly rather than reading past the array");
    free(out);

    printf("[mp_physics_test] === 7: cleanup ===\n");
    phi_physics_remove_body(world, obj.phys_body);
    phi_physics_world_destroy(world);
    halfedge_destroy(hem);
    check(1, "torn down without crashing");

    if (g_fail) printf("\n[mp_physics_test] RESULT: FAIL\n");
    else printf("\n[mp_physics_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
