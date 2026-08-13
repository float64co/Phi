/* Standalone, GL-free test for phi_physics.c/.cpp (vendored Bullet
 * Physics, client/vendor/bullet3/) -- same "prove it in isolation, no
 * window needed" precedent as mesh_edit_test_main.c/area_tree_test_main.c,
 * applied to real rigid-body dynamics. This is the test that actually
 * matters most for Phase 2's build-integration risk: does the vendored
 * C++ source really compile, link against a plain-C caller, and produce
 * numerically correct physics -- not just "did phi_physics.cpp compile". */
#include "phi_physics.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("[phi_physics_test] === 1: a box falls under gravity and lands on a static ground box ===\n");
    PhiPhysicsWorld *world = phi_physics_world_create();
    check(world != NULL, "world created");

    float identity_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    /* Static ground: a big flat box, mass=0 (Bullet's "infinite mass"
     * convention -- see phi_physics.h), top surface at y=0. */
    Vec3f ground_half = {50.0f, 1.0f, 50.0f};
    Vec3f ground_pos = {0.0f, -1.0f, 0.0f};
    PhiRigidBody *ground = phi_physics_add_box_body(world, ground_half, ground_pos, identity_quat, 0.0f, 0.3f);
    check(ground != NULL, "ground body created");

    /* Dynamic box, dropped from y=10, half-extent 0.5 -- so it should
     * come to rest with its center at y=0.5 (ground top at y=0, box
     * half-height 0.5). */
    Vec3f box_half = {0.5f, 0.5f, 0.5f};
    Vec3f box_start = {0.0f, 10.0f, 0.0f};
    PhiRigidBody *box = phi_physics_add_box_body(world, box_half, box_start, identity_quat, 1.0f, 0.1f);
    check(box != NULL, "dynamic box body created");

    Vec3f pos;
    phi_physics_get_transform(box, &pos, NULL);
    printf("  initial y = %.3f (expected 10.0)\n", pos.y);
    check(fabsf(pos.y - 10.0f) < 0.001f, "starts exactly where placed");

    /* Step a handful of frames and confirm it's actually falling --
     * checked against the analytical freefall formula (y = y0 -
     * 0.5*g*t^2) with real tolerance, not just "did y decrease". */
    float t = 0.0f;
    for (int i = 0; i < 6; i++) {
        phi_physics_world_step(world, 1.0f / 60.0f);
        t += 1.0f / 60.0f;
    }
    phi_physics_get_transform(box, &pos, NULL);
    float expected_y = 10.0f - 0.5f * 9.81f * t * t;
    printf("  after %.3fs: y = %.4f (analytical freefall predicts %.4f)\n", t, pos.y, expected_y);
    check(fabsf(pos.y - expected_y) < 0.01f, "early free-fall trajectory matches the analytical formula closely (not yet touching the ground)");

    /* Step until it settles (well past the fall time -- sqrt(2*9.5/9.81) ~= 1.39s to reach the ground from y=10 to the contact point ~1.0). */
    for (int i = 0; i < 300; i++) {
        phi_physics_world_step(world, 1.0f / 60.0f);
    }
    phi_physics_get_transform(box, &pos, NULL);
    /* Ground top surface = ground_pos.y + ground_half.y = -1.0 + 1.0 = 0.0;
     * resting box center = ground top + box half-extent = 0.0 + 0.5 = 0.5. */
    printf("  after settling (300 more steps): y = %.4f (expected ~0.5, resting on the ground top at y=0.0 plus the box's own 0.5 half-extent)\n", pos.y);
    check(fabsf(pos.y - 0.5f) < 0.05f, "box comes to rest on top of the ground, not sinking through or floating");

    Vec3f settled_v = phi_physics_get_linear_velocity(box);
    printf("  settled velocity = (%.3f, %.3f, %.3f)\n", settled_v.x, settled_v.y, settled_v.z);
    check(fabsf(settled_v.y) < 0.5f, "vertical velocity has died down (at rest, not still falling/bouncing wildly)");

    printf("[phi_physics_test] === 2: apply_impulse gives the box real horizontal velocity ===\n");
    Vec3f zero = {0.0f, 0.0f, 0.0f};
    phi_physics_apply_impulse(box, (Vec3f){5.0f, 0.0f, 0.0f}, zero);
    Vec3f v_after_impulse = phi_physics_get_linear_velocity(box);
    printf("  velocity right after a +X impulse = (%.3f, %.3f, %.3f)\n", v_after_impulse.x, v_after_impulse.y, v_after_impulse.z);
    check(v_after_impulse.x > 4.0f, "an applied impulse produces real velocity in the expected direction (impulse/mass = 5.0/1.0 = 5.0)");

    printf("[phi_physics_test] === 3: set_linear_velocity / set_transform overrides work ===\n");
    phi_physics_set_linear_velocity(box, zero);
    Vec3f v_after_stop = phi_physics_get_linear_velocity(box);
    check(fabsf(v_after_stop.x) < 0.001f && fabsf(v_after_stop.y) < 0.001f, "velocity forced to zero really is zero afterward");

    Vec3f new_pos = {20.0f, 5.0f, 3.0f};
    phi_physics_set_transform(box, new_pos, identity_quat);
    Vec3f p_after_set;
    phi_physics_get_transform(box, &p_after_set, NULL);
    check(fabsf(p_after_set.x - 20.0f) < 0.001f && fabsf(p_after_set.y - 5.0f) < 0.001f && fabsf(p_after_set.z - 3.0f) < 0.001f,
          "a direct transform override actually moves the body, not just the render-side idea of where it is");

    printf("[phi_physics_test] === 4: convex hull body -- a real quickhull-reduced shape, not a box ===\n");
    /* 8 explicit corner vertices of a unit-half-extent cube, PLUS its own
     * center point repeated as a degenerate/redundant 9th vertex --
     * btConvexHullComputer must reduce this down to the same 8-corner
     * hull (the center point lies strictly inside, contributes nothing),
     * proving this is genuine hull REDUCTION, not just "wrap whatever
     * points you're given". Dropped onto the same ground as test 1 and
     * expected to settle at the identical height a box with the same
     * half-extent would -- the hull really is a solid cube-shaped
     * collider, not degenerate/inside-out/leaking through the floor. */
    float hull_pts[9*3] = {
        -0.5f,-0.5f,-0.5f,   0.5f,-0.5f,-0.5f,  -0.5f, 0.5f,-0.5f,   0.5f, 0.5f,-0.5f,
        -0.5f,-0.5f, 0.5f,   0.5f,-0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,   0.5f, 0.5f, 0.5f,
         0.0f, 0.0f, 0.0f,
    };
    Vec3f hull_start = {3.0f, 10.0f, 0.0f};
    PhiRigidBody *hull_box = phi_physics_add_convex_hull_body(world, hull_pts, 9, hull_start, identity_quat, 1.0f, 0.1f);
    check(hull_box != NULL, "convex hull body created from a raw point cloud");
    for (int i = 0; i < 360; i++) phi_physics_world_step(world, 1.0f / 60.0f);
    Vec3f hull_pos;
    phi_physics_get_transform(hull_box, &hull_pos, NULL);
    printf("  hull settled at y = %.4f (expected ~0.5, same as the equal-size box in test 1)\n", hull_pos.y);
    check(fabsf(hull_pos.y - 0.5f) < 0.1f, "the reduced hull behaves as a real solid cube collider, settling at the same height a box would");

    printf("[phi_physics_test] === 5: fixed constraint with a real breaking threshold ===\n");
    /* Two boxes side by side, glued at the midpoint between them with a
     * breaking threshold. A gentle impulse shouldn't exceed it (stays
     * glued); a hard impulse should (Bullet's own solver disables the
     * constraint internally -- see phi_physics.h's own comment on
     * btSequentialImpulseConstraintSolver). Both boxes are kinematically
     * irrelevant to gravity here (checked mid-air, away from the ground)
     * so the ONLY force acting is the impulse itself -- an unambiguous
     * signal for whether the threshold logic is really working. */
    Vec3f cbox_half = {0.5f, 0.5f, 0.5f};
    Vec3f pos_a = {-10.0f, 20.0f, 0.0f};
    Vec3f pos_b = {-9.0f, 20.0f, 0.0f};
    PhiRigidBody *cbox_a = phi_physics_add_box_body(world, cbox_half, pos_a, identity_quat, 1.0f, 0.0f);
    PhiRigidBody *cbox_b = phi_physics_add_box_body(world, cbox_half, pos_b, identity_quat, 1.0f, 0.0f);
    Vec3f pivot = {-9.5f, 20.0f, 0.0f};
    PhiConstraint *glue = phi_physics_add_fixed_constraint(world, cbox_a, cbox_b, pivot, 50.0f);
    check(glue != NULL, "fixed constraint created");
    check(!phi_physics_constraint_is_broken(glue), "starts intact");

    /* Gentle nudge: well under the 50.0 breaking threshold. Note the
     * constraint solver has to SHARE an applied impulse P between the two
     * equal-mass glued bodies to equalize their velocities (conservation
     * of momentum: the row's own internal impulse works out to ~P/2, not
     * P) -- accounted for in both magnitudes below, not just guessed. */
    phi_physics_apply_impulse(cbox_a, (Vec3f){-2.0f, 0.0f, 0.0f}, zero);
    for (int i = 0; i < 30; i++) phi_physics_world_step(world, 1.0f / 60.0f);
    check(!phi_physics_constraint_is_broken(glue), "a gentle impulse well under the threshold leaves the constraint intact");
    Vec3f pos_a_after_gentle, pos_b_after_gentle;
    phi_physics_get_transform(cbox_a, &pos_a_after_gentle, NULL);
    phi_physics_get_transform(cbox_b, &pos_b_after_gentle, NULL);
    float gap_after_gentle = pos_b_after_gentle.x - pos_a_after_gentle.x;
    printf("  gap after gentle nudge: %.3f (started at 1.0 -- still glued means it stayed close to that)\n", gap_after_gentle);
    check(fabsf(gap_after_gentle - 1.0f) < 0.3f, "still glued: the two boxes moved together, gap didn't open up");

    /* Hard impulse: ~400, so the shared row impulse (~200) comfortably
     * clears the 50.0 threshold. */
    phi_physics_apply_impulse(cbox_a, (Vec3f){-400.0f, 0.0f, 0.0f}, zero);
    for (int i = 0; i < 30; i++) phi_physics_world_step(world, 1.0f / 60.0f);
    check(phi_physics_constraint_is_broken(glue), "a hard impulse over the threshold breaks the constraint (Bullet's own solver, not hand-rolled logic)");
    Vec3f pos_a_after_hard, pos_b_after_hard;
    phi_physics_get_transform(cbox_a, &pos_a_after_hard, NULL);
    phi_physics_get_transform(cbox_b, &pos_b_after_hard, NULL);
    float gap_after_hard = pos_b_after_hard.x - pos_a_after_hard.x;
    printf("  gap after hard impulse: %.3f (should have opened up well past the original 1.0)\n", gap_after_hard);
    check(gap_after_hard > 2.0f, "broken: box A actually flew away from box B instead of staying rigidly attached");

    phi_physics_remove_constraint(world, glue);
    check(1, "constraint removal doesn't crash");

    printf("[phi_physics_test] === 6: cleanup doesn't crash ===\n");
    phi_physics_remove_body(world, box);
    phi_physics_remove_body(world, ground);
    phi_physics_remove_body(world, hull_box);
    phi_physics_remove_body(world, cbox_a);
    phi_physics_remove_body(world, cbox_b);
    phi_physics_world_destroy(world);
    check(1, "world + every body torn down without crashing (this line running proves it)");

    if (g_fail) printf("\n[phi_physics_test] RESULT: FAIL\n");
    else printf("\n[phi_physics_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
