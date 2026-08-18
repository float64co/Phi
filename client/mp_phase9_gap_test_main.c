/* Standalone MicroPython integration test for the Phase 9 gap-closing
 * bindings (2026-08-18, see phi.md's Phase 9 "Known gaps": assessed
 * directly against "hand Phi some glTF assets today, build a real game"
 * and found no camera control, no whole-object transform API, no
 * keyboard/mouse input, and no Python physics/gamepad access in the
 * player build). Exercises every one of those against a REAL embedded
 * interpreter, same "prove it end to end, no GL/window needed" precedent
 * as mp_physics_test_main.c/mp_node_test_main.c, not a mock of either.
 *
 * Camera and gamepad are function-pointer handoffs (see mp_port.h's own
 * comment on why: mp_port.c must never need renderer.c/GL or input_
 * gamepad_native.c/SDL2 linked in just for this test to build) -- this
 * test provides small stand-in callbacks the same way test_get_selected_
 * object below already stands in for main.c's real selected_mesh_
 * object(). Keyboard/mouse input needs no such indirection (mp_port.c
 * only ever reads InputState's fields, never calls into input.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mp_port.h"
#include "scene_objects.h"
#include "halfedge_gltf.h"
#include "phi_physics.h"
#include "input.h"
#include "input_gamepad.h"

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

/* Same real-free, not-a-no-op stub every other mp_*_test harness that
 * links scene_objects.c already establishes -- see mp_geometry_test_
 * main.c's own comment for why this is needed regardless of whether this
 * test's own calls ever reach it. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

/* ---- Camera stand-in: records whatever phi.set_camera last passed,
 * exactly the shape player_main.c's own real wrapper (calling the real
 * renderer_set_camera) will have, just without a real Renderer. ---- */
static Vec3f s_last_cam_eye;
static float s_last_cam_yaw, s_last_cam_pitch;
static int   s_cam_call_count = 0;
static void test_set_camera(Vec3f eye, float yaw, float pitch) {
    s_last_cam_eye = eye;
    s_last_cam_yaw = yaw;
    s_last_cam_pitch = pitch;
    s_cam_call_count++;
}

/* ---- Gamepad stand-in: a fully C-controlled fake gamepad registry, same
 * shape phi_gamepad_count/phi_gamepad_get_state have in the real
 * input_gamepad.h, just with test-controlled contents instead of a real
 * SDL2/browser backend underneath. ---- */
static PhiGamepadState s_fake_pads[PHI_GAMEPAD_MAX];
static int test_gamepad_count(void) {
    int n = 0;
    for (int i = 0; i < PHI_GAMEPAD_MAX; i++) if (s_fake_pads[i].connected) n++;
    return n;
}
static const PhiGamepadState *test_gamepad_get_state(int index) {
    if (index < 0 || index >= PHI_GAMEPAD_MAX) return NULL;
    return &s_fake_pads[index];
}

int main(void) {
    int stack_top;

    printf("[mp_phase9_gap_test] === setup ===\n");
    scene_objects_init();
    phi_mp_init(&stack_top);

    if (g_fail) { printf("\n[mp_phase9_gap_test] RESULT: FAIL (setup)\n"); return 1; }

    printf("[mp_phase9_gap_test] === 1: phi.set_camera before any registration raises cleanly ===\n");
    char *out = phi_mp_exec("phi.set_camera(1.0, 2.0, 3.0, 0.5, -0.2)");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "unregistered camera callback raises ValueError, not a crash");
    free(out);

    printf("[mp_phase9_gap_test] === 2: phi.set_camera really calls the registered callback with the real arguments ===\n");
    phi_mp_register_camera_callback(test_set_camera);
    out = phi_mp_exec("phi.set_camera(10.0, 20.0, 30.0, 1.5, -0.75)");
    printf("  captured: %s", out);
    check(s_cam_call_count == 1, "the registered camera callback was actually invoked exactly once");
    check(s_last_cam_eye.x == 10.0f && s_last_cam_eye.y == 20.0f && s_last_cam_eye.z == 30.0f, "eye position passed through correctly");
    check(s_last_cam_yaw == 1.5f && s_last_cam_pitch == -0.75f, "yaw/pitch passed through correctly");
    free(out);

    printf("[mp_phase9_gap_test] === 3: whole-object transform get/set, keyed by object id ===\n");
    MeshObject *obj = scene_object_add();
    check(obj != NULL, "scene_object_add() gave a real slot");
    obj->position = (Vec3f){1.0f, 2.0f, 3.0f};
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "print(phi.get_object_position(%d))", obj->id);
    out = phi_mp_exec(cmd);
    printf("  captured: %s", out);
    check(strstr(out, "1.0") != NULL && strstr(out, "2.0") != NULL && strstr(out, "3.0") != NULL, "get_object_position reads the real, current C-side position");
    free(out);

    snprintf(cmd, sizeof(cmd), "phi.set_object_position(%d, 40.0, 50.0, 60.0)", obj->id);
    out = phi_mp_exec(cmd);
    free(out);
    check(obj->position.x == 40.0f && obj->position.y == 50.0f && obj->position.z == 60.0f, "set_object_position really wrote the C-side MeshObject's position");

    snprintf(cmd, sizeof(cmd), "phi.set_object_rotation(%d, 0.0, 0.707, 0.0, 0.707)", obj->id);
    out = phi_mp_exec(cmd);
    free(out);
    check(fabsf(obj->orientation.y - 0.707f) < 0.001f && fabsf(obj->orientation.w - 0.707f) < 0.001f, "set_object_rotation really wrote the C-side quaternion");

    snprintf(cmd, sizeof(cmd), "print(phi.get_object_rotation(%d))", obj->id);
    out = phi_mp_exec(cmd);
    printf("  captured: %s", out);
    check(strstr(out, "0.707") != NULL, "get_object_rotation reads it back");
    free(out);

    snprintf(cmd, sizeof(cmd), "phi.set_object_scale(%d, 2.0, 3.0, 4.0)", obj->id);
    out = phi_mp_exec(cmd);
    free(out);
    check(obj->scale.x == 2.0f && obj->scale.y == 3.0f && obj->scale.z == 4.0f, "set_object_scale really wrote the C-side scale");

    out = phi_mp_exec("phi.set_object_scale(999999, 1.0, 1.0, 1.0)");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_object_scale on a nonexistent object id raises rather than crashing");
    free(out);

    out = phi_mp_exec("phi.set_object_scale(1, 0.0, 1.0, 1.0)");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_object_scale rejects a zero component (a real, previously-hit degenerate case elsewhere in this codebase)");
    free(out);

    printf("[mp_phase9_gap_test] === 4: object-id-keyed physics (no selection registered at all) ===\n");
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf");
    for (int i = 0; i < hem->vert_count; i++)
        for (int a = 0; a < 3; a++)
            hem->verts[i].pos[a] *= 16.0f;
    obj->hem = hem;
    obj->position = (Vec3f){0.0f, 100.0f, 0.0f};
    obj->orientation = quat_identity();

    PhiPhysicsWorld *world = phi_physics_world_create();
    float identity_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    phi_physics_add_box_body(world, (Vec3f){200.0f, 10.0f, 200.0f}, (Vec3f){0.0f, 0.0f, 0.0f}, identity_quat, 0.0f, 0.3f);
    phi_mp_register_physics_world(world);

    out = phi_mp_exec("phi.object_apply_impulse(999999, (1,0,0), (0,0,0))");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "object_apply_impulse on a nonexistent object id raises cleanly");
    free(out);

    check(obj->phys_body == NULL, "no phys_body before the call");
    snprintf(cmd, sizeof(cmd), "phi.object_enable_physics(%d, 1.0, 0.3)", obj->id);
    out = phi_mp_exec(cmd);
    printf("  captured: %s", out);
    free(out);
    check(obj->phys_body != NULL, "object_enable_physics (no selection involved at all) really created a C-side Bullet body");

    float start_y = obj->position.y;
    for (int i = 0; i < 120; i++) {
        phi_physics_world_step(world, 1.0f / 60.0f);
        float o[4];
        phi_physics_get_transform(obj->phys_body, &obj->position, o);
    }
    printf("  y: %.2f -> %.2f after 2s of simulated falling\n", start_y, obj->position.y);
    check(obj->position.y < start_y - 5.0f, "the object created via the object-id-keyed Python physics binding actually falls under real gravity");

    snprintf(cmd, sizeof(cmd), "phi.object_set_velocity(%d, (0.0, 0.0, 0.0))\nphi.object_apply_impulse(%d, (5.0, 0.0, 0.0), (0.0, 0.0, 0.0))\nprint(phi.object_get_velocity(%d))",
             obj->id, obj->id, obj->id);
    out = phi_mp_exec(cmd);
    printf("  captured: %s", out);
    check(strstr(out, "5.0") != NULL, "object_set_velocity/object_apply_impulse/object_get_velocity round-trip correctly (impulse/mass = 5.0/1.0)");
    free(out);

    printf("[mp_phase9_gap_test] === 5: keyboard/mouse input before registration raises cleanly ===\n");
    out = phi_mp_exec("phi.key_down('w')");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "phi.key_down before phi_mp_register_input raises rather than segfaulting on a NULL pointer");
    free(out);

    printf("[mp_phase9_gap_test] === 6: keyboard/mouse input reflects a real, registered InputState ===\n");
    /* Zeroed directly rather than via input_init() -- input_init just
     * memsets, but linking input.c itself would pull in real X11 (this
     * platform's backend) just for that one trivial call, the same real
     * problem mp_port.h's own comment on phi_mp_register_camera_callback/
     * _gamepad_callbacks explains for renderer.c/input_gamepad_native.c.
     * input.c's own platform-specific key-mapping logic (x11_keysym_to_
     * phikey etc.) is verified separately, by real interactive use of the
     * actual player build -- there's no dedicated unit test for it, same
     * as every other platform event-callback code in this codebase. */
    InputState inp;
    memset(&inp, 0, sizeof(inp));
    phi_mp_register_input(&inp);

    out = phi_mp_exec("print(phi.key_down('w'))");
    check(strstr(out, "False") != NULL, "key_down('w') is False when nothing is held");
    free(out);

    inp.keys_down[PHI_KEY_W] = 1;
    out = phi_mp_exec("print(phi.key_down('w'))");
    printf("  captured: %s", out);
    check(strstr(out, "True") != NULL, "key_down('w') reflects the real, live InputState the instant it changes -- read fresh every call, not snapshotted");
    free(out);
    inp.keys_down[PHI_KEY_W] = 0;

    inp.keys_down[PHI_KEY_SPACE] = 1;
    out = phi_mp_exec("print(phi.key_down('space'))");
    check(strstr(out, "True") != NULL, "named keys (not just single letters) resolve correctly");
    free(out);

    out = phi_mp_exec("phi.key_down('not_a_real_key')");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "an unrecognized key name raises rather than silently returning False");
    free(out);

    inp.mouse_x = 123; inp.mouse_y = 456;
    out = phi_mp_exec("print(phi.mouse_pos())");
    printf("  captured: %s", out);
    check(strstr(out, "123") != NULL && strstr(out, "456") != NULL, "mouse_pos() reflects the real, live cursor position");
    free(out);

    inp.lmb_down = 1;
    out = phi_mp_exec("print(phi.mouse_button_down('left'), phi.mouse_button_down('right'))");
    printf("  captured: %s", out);
    check(strstr(out, "True False") != NULL, "mouse_button_down distinguishes held vs. not-held per button");
    free(out);

    out = phi_mp_exec("phi.mouse_button_down('nonsense')");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "an unrecognized mouse button name raises");
    free(out);

    printf("[mp_phase9_gap_test] === 7: gamepad before registration is honestly '0 connected', not a crash ===\n");
    out = phi_mp_exec("print(phi.gamepad_count())");
    printf("  captured: %s", out);
    check(strstr(out, "0") != NULL, "gamepad_count() is 0 with no callback registered");
    free(out);
    out = phi_mp_exec("print(phi.gamepad_connected(0))");
    check(strstr(out, "False") != NULL, "gamepad_connected(0) is False with no callback registered");
    free(out);

    printf("[mp_phase9_gap_test] === 8: gamepad reflects a real, registered backend ===\n");
    memset(s_fake_pads, 0, sizeof(s_fake_pads));
    s_fake_pads[0].connected = 1;
    strcpy(s_fake_pads[0].name, "Fake Test Pad");
    s_fake_pads[0].buttons[PHI_GAMEPAD_BUTTON_A] = 1;
    s_fake_pads[0].axes[PHI_GAMEPAD_AXIS_LEFTX] = 0.75f;
    phi_mp_register_gamepad_callbacks(test_gamepad_count, test_gamepad_get_state);

    out = phi_mp_exec("print(phi.gamepad_count())");
    printf("  captured: %s", out);
    check(strstr(out, "1") != NULL, "gamepad_count() now reflects the real registered backend");
    free(out);

    out = phi_mp_exec("print(phi.gamepad_connected(0), phi.gamepad_connected(1))");
    printf("  captured: %s", out);
    check(strstr(out, "True False") != NULL, "gamepad_connected distinguishes slot 0 (connected) from slot 1 (not)");
    free(out);

    out = phi_mp_exec("print(phi.gamepad_button(0, 'a'), phi.gamepad_button(0, 'b'))");
    printf("  captured: %s", out);
    check(strstr(out, "True False") != NULL, "gamepad_button reads the real per-button state");
    free(out);

    out = phi_mp_exec("print(phi.gamepad_axis(0, 'leftx'))");
    printf("  captured: %s", out);
    check(strstr(out, "0.75") != NULL, "gamepad_axis reads the real axis value");
    free(out);

    out = phi_mp_exec("phi.gamepad_button(0, 'not_a_real_button')");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "an unrecognized button name raises");
    free(out);

    out = phi_mp_exec("print(phi.gamepad_button(1, 'a'))");
    printf("  captured: %s", out);
    check(strstr(out, "False") != NULL, "a disconnected slot's button reads as False, not an error -- consistent with input_gamepad.h's own 'safe, just meaningless' convention");
    free(out);

    printf("[mp_phase9_gap_test] === 9: cleanup ===\n");
    phi_physics_remove_body(world, obj->phys_body);
    obj->phys_body = NULL;   /* already freed above -- scene_object_delete must not try to remove it again */
    phi_physics_world_destroy(world);
    scene_object_delete(obj, NULL);
    check(1, "torn down without crashing");

    if (g_fail) printf("\n[mp_phase9_gap_test] RESULT: FAIL\n");
    else printf("\n[mp_phase9_gap_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
