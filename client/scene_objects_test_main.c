/* Standalone, GL-free test harness for scene_objects.c -- Phase 5's real
 * multi-object scene graph (see scene_objects.h), replacing the single
 * g_test_mesh_object slot every earlier phase this session built around.
 * No stub functions needed for THIS test's own logic (scene_object_add/
 * find/get_all/count never touch GL), but scene_object_delete calls
 * mesh_destroy (octree_render.c's real GL-touching home) -- same stub
 * technique fracture_body_test_main.c already established. */
#include "scene_objects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

/* Real free(), not a no-op -- see fracture_body_test_main.c's identical
 * stub for the full "why octree_render.c isn't linked here" reasoning. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

/* Stub -- scene_object_delete references phi_physics_remove_body
 * (phi_physics.cpp/Bullet, a much heavier real dependency than this
 * registry-semantics test has any reason to link) regardless of whether
 * this test's own calls ever reach it at runtime (every scene_object_
 * delete call below passes phys_world=NULL, so the real body is never
 * live) -- the SYMBOL still needs to resolve at link time. A no-op is
 * honest here (unlike mesh_destroy's real free() above): this test
 * never gives any object a real phys_body, so there is nothing for a
 * real implementation to actually do. */
void phi_physics_remove_body(PhiPhysicsWorld *world, PhiRigidBody *body) {
    (void)world; (void)body;
}

int main(void) {
    printf("[scene_objects_test] === setup ===\n");
    scene_objects_init();
    check(scene_object_count() == 0, "registry starts empty");

    printf("[scene_objects_test] === 1: add/find/get_all ===\n");
    MeshObject *a = scene_object_add();
    MeshObject *b = scene_object_add();
    check(a != NULL && b != NULL, "two real slots allocated");
    check(a->id != b->id, "distinct ids assigned");
    check(scene_object_count() == 2, "count reflects both live objects");
    check(scene_object_find(a->id) == a, "find resolves back to the exact same pointer");
    check(scene_object_find(b->id) == b, "find resolves the second object too");
    check(scene_object_find(999999) == NULL, "find fails cleanly for a nonexistent id");

    MeshObject *all[SCENE_MAX_OBJECTS];
    int n = scene_object_get_all(all);
    check(n == 2, "get_all reports exactly the 2 live objects");
    check((all[0] == a && all[1] == b) || (all[0] == b && all[1] == a), "get_all's two pointers are exactly a and b");

    printf("[scene_objects_test] === 2: address stability -- pointers never move ===\n");
    /* A fixed array of slots (see scene_objects.h's own comment on why
     * this matters: Properties/Python/scene_target all hold onto a
     * MeshObject* across frames) -- adding MORE objects must not
     * invalidate a/b's own addresses, unlike a realloc'd dynamic array. */
    MeshObject *c = scene_object_add();
    check(c != NULL, "a third object added");
    check(scene_object_find(a->id) == a && scene_object_find(b->id) == b,
          "a/b's addresses are unchanged after a later add (no reallocation happened)");

    printf("[scene_objects_test] === 3: delete frees the slot, id never reused ===\n");
    int deleted_id = b->id;
    scene_object_delete(b, NULL);   /* NULL phys_world -- b never got a physics body in this test */
    check(scene_object_count() == 2, "count drops back to 2 after deleting one of three");
    check(scene_object_find(deleted_id) == NULL, "the deleted object's id no longer resolves");
    MeshObject *d = scene_object_add();
    check(d != NULL, "a fresh slot can be allocated after a delete (the freed slot is reusable)");
    check(d->id != deleted_id && d->id != a->id && d->id != c->id,
          "the new object gets a genuinely fresh id, never reusing the deleted one (monotonic, see scene_objects.c's s_next_id)");
    scene_object_delete(NULL, NULL);
    check(1, "deleting NULL is a safe no-op (this line running proves it)");

    printf("[scene_objects_test] === 4: registry-full behavior ===\n");
    scene_objects_init();   /* fresh registry for a clean capacity test */
    int spawned = 0;
    for (int i = 0; i < SCENE_MAX_OBJECTS + 4; i++) {
        if (scene_object_add()) spawned++;
    }
    check(spawned == SCENE_MAX_OBJECTS, "can fill the registry to exactly its declared capacity, no further");
    check(scene_object_count() == SCENE_MAX_OBJECTS, "count matches the real capacity, not more");
    MeshObject *full_all[SCENE_MAX_OBJECTS];
    check(scene_object_get_all(full_all) == SCENE_MAX_OBJECTS, "get_all still reports exactly SCENE_MAX_OBJECTS, not a buffer overrun");

    printf("\n[scene_objects_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
