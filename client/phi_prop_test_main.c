/* Standalone, GL-free test harness for phi_prop.c/phi_prop_registry.c --
 * same precedent as area_tree_test_main.c/mesh_edit_test_main.c: this
 * subsystem has zero GL dependency, so it's verified in isolation rather
 * than only reachable through a live window this sandbox can't open. */
#include "phi_prop_registry.h"
#include "meshobject.h"
#include "halfedge.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("[phi_prop_test] === 1: phi_prop_find ===\n");
    const PhiProp *pos = phi_prop_find(&g_phi_prop_mesh_object, "position");
    check(pos != NULL && strcmp(pos->identifier, "position") == 0, "finds a real property by identifier");
    check(phi_prop_find(&g_phi_prop_mesh_object, "not_a_real_prop") == NULL, "unknown identifier returns NULL");

    printf("[phi_prop_test] === 2: MeshObject.position (VEC3) get/set through the real struct ===\n");
    MeshObject obj = {0};
    obj.position = (Vec3f){1.0f, 2.0f, 3.0f};
    float v[3];
    check(phi_prop_get_vec3(&obj, pos, v) == 1, "get_vec3 succeeds on a real VEC3 prop");
    check(v[0] == 1.0f && v[1] == 2.0f && v[2] == 3.0f, "reads the actual struct field, not a stale copy");
    float newv[3] = {10.0f, 20.0f, 30.0f};
    check(phi_prop_set_vec3(&obj, pos, newv) == 1, "set_vec3 succeeds");
    check(obj.position.x == 10.0f && obj.position.y == 20.0f && obj.position.z == 30.0f,
          "the real struct field actually changed (offsetof-based write lands in the right place)");
    check(phi_prop_get_float(&obj, pos, &v[0]) == 0, "get_float on a VEC3 prop is a type-mismatch, not a silent wrong read");

    printf("[phi_prop_test] === 3: MeshObject.is_static (BOOL, stored as C int) ===\n");
    const PhiProp *is_static = phi_prop_find(&g_phi_prop_mesh_object, "is_static");
    obj.is_static = 1;
    float sv;
    check(phi_prop_get_float(&obj, is_static, &sv) == 1 && sv == 1.0f, "BOOL reads through the float accessor as 1.0");
    check(phi_prop_set_float(&obj, is_static, 0.0f) == 1 && obj.is_static == 0, "set_float(0.0) on a BOOL clears the real int field");
    check(phi_prop_set_float(&obj, is_static, 5.0f) == 1 && obj.is_static == 1, "any nonzero float sets a BOOL true, not literally 5");

    printf("[phi_prop_test] === 4: HEFace.metallic/roughness range clamping ===\n");
    HEFace face = {0};
    const PhiProp *metallic = phi_prop_find(&g_phi_prop_heface, "metallic");
    check(metallic != NULL && metallic->range[0] == 0.0f && metallic->range[1] == 1.0f, "metallic registered with a real [0,1] range");
    phi_prop_set_float(&face, metallic, 1.5f);
    check(face.metallic == 1.0f, "set_float clamps above the range max");
    phi_prop_set_float(&face, metallic, -0.5f);
    check(face.metallic == 0.0f, "set_float clamps below the range min");
    phi_prop_set_float(&face, metallic, 0.42f);
    check(face.metallic == 0.42f, "an in-range value passes through unclamped");

    printf("[phi_prop_test] === 5: HEFace.base_color/emission are deliberately unclamped (matches halfedge_set_face_material's own behavior) ===\n");
    const PhiProp *base_color = phi_prop_find(&g_phi_prop_heface, "base_color");
    check(base_color->range[0] == 0.0f && base_color->range[1] == 0.0f, "range[0]==range[1] means unbounded, not \"clamped to exactly 0\"");
    float hot[3] = {5.0f, 5.0f, 5.0f};
    phi_prop_set_vec3(&face, base_color, hot);
    check(face.base_color[0] == 5.0f, "an out-of-[0,1] value is accepted verbatim, not silently clamped");

    printf("[phi_prop_test] === 6: HEFace.emission round-trip ===\n");
    const PhiProp *emission = phi_prop_find(&g_phi_prop_heface, "emission");
    float em_in[3] = {2.0f, 0.0f, 0.0f}, em_out[3];
    phi_prop_set_vec3(&face, emission, em_in);
    phi_prop_get_vec3(&face, emission, em_out);
    check(em_out[0] == 2.0f && em_out[1] == 0.0f && em_out[2] == 0.0f, "emission round-trips exactly, including a >1.0 value used for bloom");

    if (g_fail) printf("\n[phi_prop_test] RESULT: FAIL\n");
    else printf("\n[phi_prop_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
