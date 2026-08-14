/* Standalone, GL-free test harness for light.c (Light objects, Phase 3)
 * and scene_target.c (the shared "object"/"face"/"light:<id>"/"render"
 * resolver) -- same no-GL-dependency precedent as area_tree_test_main.c/
 * phi_prop_test_main.c: neither subsystem touches OpenGL at all, so both
 * are verified in isolation here rather than only reachable through a
 * live window this sandbox can't open. */
#include "light.h"
#include "scene_target.h"
#include "meshobject.h"
#include "halfedge.h"
#include "halfedge_gltf.h"
#include "render_settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* scene_target_register now takes a resolver CALLBACK (Phase 5's real
 * multi-object scene graph, see scene_objects.h/scene_target.h) rather
 * than a fixed MeshObject*+loaded-bool pair -- this test's own stand-in
 * for main.c's selected_mesh_object(), toggled directly by the test
 * below (NULL = "nothing selected", same as an empty scene). */
static MeshObject *s_test_selected_obj = NULL;
static MeshObject *test_get_selected_object(void) { return s_test_selected_obj; }

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("[light_test] === 1: spawn/find/delete ===\n");
    light_system_init();
    PhiLight *a = light_spawn(LIGHT_TYPE_POINT, (Vec3f){1.0f, 2.0f, 3.0f});
    check(a != NULL, "spawn returns a real light");
    check(a->id > 0, "assigned a real, positive id");
    check(a->position.x == 1.0f && a->position.y == 2.0f && a->position.z == 3.0f, "position lands correctly");
    check(a->color.x == 1.0f && a->color.y == 1.0f && a->color.z == 1.0f, "default color is white");
    check(a->energy > 0.0f, "default energy is a real positive value, not zero/uninitialized");

    PhiLight *b = light_spawn(LIGHT_TYPE_SUN, (Vec3f){0.0f, 0.0f, 0.0f});
    check(b != NULL && b->id != a->id, "a second light gets a distinct id");

    check(light_find(a->id) == a, "find returns the same pointer spawn gave back");
    check(light_find(99999) == NULL, "find on a nonexistent id returns NULL");

    int a_id = a->id;
    check(light_delete(a_id) == 1, "delete reports success for a real light");
    check(light_find(a_id) == NULL, "the deleted light is really gone");
    check(light_delete(a_id) == 0, "deleting the same id twice reports failure, not a crash");

    printf("[light_test] === 2: get_all reflects exactly the live set ===\n");
    PhiLight *all[PHI_MAX_LIGHTS];
    int n = light_get_all(all);
    check(n == 1, "get_all returns exactly the one remaining light (a was deleted, b wasn't)");
    check(n == 1 && all[0] == b, "and it's really b, not a stale/wrong pointer");

    printf("[light_test] === 3: registry-full behavior ===\n");
    light_system_init();
    int spawned = 0;
    for (int i = 0; i < PHI_MAX_LIGHTS; i++) {
        if (light_spawn(LIGHT_TYPE_POINT, (Vec3f){(float)i, 0.0f, 0.0f})) spawned++;
    }
    check(spawned == PHI_MAX_LIGHTS, "can fill the registry to exactly its declared capacity");
    check(light_spawn(LIGHT_TYPE_POINT, (Vec3f){0,0,0}) == NULL, "one more spawn past capacity fails cleanly, not a buffer overrun");
    n = light_get_all(all);
    check(n == PHI_MAX_LIGHTS, "get_all still reports exactly PHI_MAX_LIGHTS, not more");

    printf("[light_test] === 4: ray_ray_pick -- real geometric hit-test, not a guess ===\n");
    light_system_init();
    PhiLight *target = light_spawn(LIGHT_TYPE_POINT, (Vec3f){10.0f, 0.0f, 0.0f});
    (void)target;
    Vec3f origin = {0.0f, 0.0f, 0.0f};
    Vec3f dir_hit = {1.0f, 0.0f, 0.0f};     /* aimed straight at the light */
    Vec3f dir_miss = {0.0f, 1.0f, 0.0f};    /* aimed straight up, well clear of it */
    float t;
    PhiLight *hit = light_ray_pick(origin, dir_hit, &t);
    check(hit != NULL, "a ray aimed directly at the light hits it");
    /* The nearest hit is on the NEAR SIDE of the icon sphere, not at its
     * center -- 10 units to the center minus the sphere's own radius
     * (LIGHT_ICON_RADIUS, light.c) -- a real geometric expectation, not a
     * loose guess (the earlier "close to 10.0" version of this check was
     * simply wrong about what a sphere hit-test actually returns). */
    check(hit && fabsf(t - 8.0f) < 0.5f, "hit distance matches the real ray-sphere intersection math (10 units to center minus the icon's own radius)");
    check(light_ray_pick(origin, dir_miss, &t) == NULL, "a ray aimed well clear of the light misses");

    printf("[light_test] === 5: scene_target resolver -- object/face/light/render ===\n");
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf for the object/face resolver checks");
    MeshObject obj = {0};
    obj.id = 1;
    obj.hem = hem;
    obj.scale = (Vec3f){1.0f, 1.0f, 1.0f};
    s_test_selected_obj = &obj;
    int edit_face = 0;
    RenderSettings rs = {128};
    scene_target_register(test_get_selected_object, &edit_face, &rs);

    const PhiPropGroup *group; void *owner;
    check(scene_resolve_target("object", &group, &owner) == 1 && owner == &obj, "'object' resolves to the registered MeshObject");
    check(scene_resolve_target("face", &group, &owner) == 1 && owner == &obj.hem->faces[0], "'face' resolves to the currently-selected face");
    check(scene_resolve_target("render", &group, &owner) == 1 && owner == &rs, "'render' resolves to the registered RenderSettings, unconditionally available");

    s_test_selected_obj = NULL;
    check(scene_resolve_target("object", &group, &owner) == 0, "'object' fails once nothing is selected, not stale-true");
    s_test_selected_obj = &obj;

    edit_face = -1;
    check(scene_resolve_target("face", &group, &owner) == 0, "'face' fails when no face is selected (-1)");
    edit_face = 0;

    PhiLight *l = light_spawn(LIGHT_TYPE_AREA, (Vec3f){5.0f, 5.0f, 5.0f});
    char target_str[32];
    snprintf(target_str, sizeof(target_str), "light:%d", l->id);
    check(scene_resolve_target(target_str, &group, &owner) == 1 && owner == l,
          "'light:<id>' resolves to the real light with that id, reading light.c's own registry directly (no registration needed)");
    check(scene_resolve_target("light:999999", &group, &owner) == 0, "'light:<id>' fails cleanly for a nonexistent id");
    check(scene_resolve_target("not_a_real_target", &group, &owner) == 0, "an unrecognized target string fails cleanly, not a crash");

    halfedge_destroy(hem);

    if (g_fail) { printf("\n[light_test] RESULT: FAIL\n"); return 1; }
    printf("\n[light_test] RESULT: PASS (all checks passed)\n");
    return 0;
}
