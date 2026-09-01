/* Standalone numerical verification for meshobject.c's quat_y_up_to_z_up/
 * quat_z_up_to_y_up (2026-08-19, see vec3.h's coordinate-convention note
 * and meshobject.c's own comment on these two) -- no GL, no window, same
 * "prove the math is actually right" precedent every other client/
 * *_test_main.c in this codebase follows, needed here specifically
 * because this environment can't render a live animated character to
 * eyeball whether a skeleton's rest/animated rotations survived the
 * Y-up -> Z-up conversion correctly. Real quaternion-rotate-vector math
 * (not asserted against a canned expected value) checked against the
 * ALREADY-verified position transform (vec3_y_up_to_z_up) for
 * consistency, the actual property that matters: rotating a vector by q
 * and then converting the result must equal converting the vector and q
 * separately and then rotating. */
#include <stdio.h>
#include <math.h>
#include "meshobject.h"
#include "vec3.h"

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

static Vec3f rotate_vec_by_quat(Quat q, Vec3f v) {
    /* v' = v + 2*cross(qv, cross(qv, v) + q.w*v), the standard real
     * quaternion-rotate-vector formula (independent of, and NOT reusing,
     * quat_y_up_to_z_up's own Hamilton-product-based implementation --
     * deliberately a second, differently-derived path, so this test
     * can't just be checking the code against itself). */
    Vec3f qv = { q.x, q.y, q.z };
    Vec3f t = {
        2.0f * (qv.y*v.z - qv.z*v.y),
        2.0f * (qv.z*v.x - qv.x*v.z),
        2.0f * (qv.x*v.y - qv.y*v.x),
    };
    return (Vec3f){
        v.x + q.w*t.x + (qv.y*t.z - qv.z*t.y),
        v.y + q.w*t.y + (qv.z*t.x - qv.x*t.z),
        v.z + q.w*t.z + (qv.x*t.y - qv.y*t.x),
    };
}

static Quat axis_angle(Vec3f axis, float angle_rad) {
    float len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    axis.x /= len; axis.y /= len; axis.z /= len;
    float s = sinf(angle_rad * 0.5f), c = cosf(angle_rad * 0.5f);
    return (Quat){ axis.x*s, axis.y*s, axis.z*s, c };
}

static int vec3_close(Vec3f a, Vec3f b, float eps) {
    return fabsf(a.x-b.x) < eps && fabsf(a.y-b.y) < eps && fabsf(a.z-b.z) < eps;
}

static void run_case(const char *label, Quat q, Vec3f v) {
    printf("[quat_axis_convert_test] === %s ===\n", label);
    Vec3f v_rotated = rotate_vec_by_quat(q, v);

    Quat q_conv = quat_y_up_to_z_up(q);
    Vec3f v_conv = vec3_y_up_to_z_up(v);
    Vec3f v_rotated_conv_direct = vec3_y_up_to_z_up(v_rotated);      /* convert the already-rotated result */
    Vec3f v_rotated_conv_via_q  = rotate_vec_by_quat(q_conv, v_conv); /* convert q and v separately, then rotate */

    check(vec3_close(v_rotated_conv_direct, v_rotated_conv_via_q, 1e-4f),
          "rotate-then-convert equals convert-then-rotate (q'/v' are a consistent re-expression of q/v in the new frame)");

    /* Round trip: converting q to Z-up and back to Y-up must reproduce
     * the identical rotation ACTION (q and -q represent the same
     * rotation, so compare by rotating a real test vector with both,
     * not by comparing the raw quaternion components). */
    Quat q_back = quat_z_up_to_y_up(q_conv);
    Vec3f test_v = {1.3f, -0.7f, 2.1f};
    check(vec3_close(rotate_vec_by_quat(q, test_v), rotate_vec_by_quat(q_back, test_v), 1e-4f),
          "quat_z_up_to_y_up(quat_y_up_to_z_up(q)) rotates identically to the original q");
}

int main(void) {
    printf("[quat_axis_convert_test] === sanity: converting an identity rotation stays identity ===\n");
    Quat id_conv = quat_y_up_to_z_up(quat_identity());
    Vec3f probe = {2.0f, 3.0f, 5.0f};
    check(vec3_close(rotate_vec_by_quat(id_conv, probe), probe, 1e-4f),
          "identity rotation, converted, still rotates nothing");

    run_case("90-degree yaw (about old-up Y)", axis_angle((Vec3f){0,1,0}, (float)M_PI * 0.5f), (Vec3f){1.0f, 0.0f, 0.0f});
    run_case("arbitrary axis/angle #1", axis_angle((Vec3f){0.3f, 0.5f, 0.8f}, 0.9f), (Vec3f){2.0f, -1.0f, 0.5f});
    run_case("arbitrary axis/angle #2", axis_angle((Vec3f){-0.6f, 0.2f, 0.4f}, 2.3f), (Vec3f){-3.0f, 4.0f, -2.0f});
    run_case("near-180-degree rotation", axis_angle((Vec3f){0.0f, 0.0f, 1.0f}, (float)M_PI * 0.999f), (Vec3f){1.0f, 1.0f, 1.0f});

    if (g_fail) printf("\n[quat_axis_convert_test] RESULT: FAIL\n");
    else printf("\n[quat_axis_convert_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
