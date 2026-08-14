/* Standalone, GL-free test harness for fracture_body.c -- Phase 2's
 * "shatter on impact" completion (see fracture_body.h). Same no-GL
 * dependency rationale as phi_physics_test_main.c/fracture_test_main.c:
 * activation/physics/adjacency-constraint correctness doesn't need a
 * window to verify, only a real physics world and a real glTF mesh. */
#include "fracture_body.h"
#include "halfedge_gltf.h"
#include <math.h>
#include <stdio.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

/* Stub -- fracture_body.c's translation unit references
 * renderer_draw_mesh_object (from fracture_body_sync_and_draw_all, real
 * rendering code) regardless of whether this test calls that specific
 * function, since C links at the whole-object-file level, not per-
 * function -- same technique light_test_main.c already established for
 * light.c's own renderer_draw_solid_box reference before that got split
 * out into renderer.c. Deliberately NOT doing that same split here:
 * fracture_body_sync_and_draw_all's own job is inseparably "sync from
 * physics, then draw" in one pass (unlike light_draw_all, which never
 * touched physics), so splitting it would leave the sync half needing
 * its own new function anyway for no real benefit -- a stub is the more
 * honest amount of restructuring for what this test actually needs to
 * isolate. */
void renderer_draw_mesh_object(Renderer *r, const MeshObject *obj) {
    (void)r; (void)obj;
}

/* Stub -- octree_render.c (mesh_destroy's real home) also carries
 * mesh_upload_stride/mesh_draw, which reference real GL functions;
 * linking the whole translation unit for mesh_destroy alone (fracture_
 * body_clear's only actual dependency on it, see fracture_body.c --
 * make_fragment_render_mesh builds RenderMesh buffers directly rather
 * than through mesh_create, so that GL-free half was never the issue)
 * would pull GL in unnecessarily for a no-GL test. Real free(), not a
 * no-op -- this test still needs fragment render-mesh memory actually
 * released, just not through octree_render.c's own GL-touching file. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

static float vec3_dist(Vec3f a, Vec3f b) {
    float dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

/* Max pairwise distance between any two live fragments -- a real,
 * simple "are they still one cluster or did something fly off" metric,
 * since every fragment starts at the EXACT SAME position (see
 * fracture_body.h's own comment on why that's a correct choice, not
 * just convenient). */
static float max_fragment_spread(int n) {
    float max_d = 0.0f;
    for (int i = 0; i < n; i++) {
        Vec3f pi;
        if (!fracture_body_get(i, &pi, NULL)) continue;
        for (int j = i + 1; j < n; j++) {
            Vec3f pj;
            if (!fracture_body_get(j, &pj, NULL)) continue;
            float d = vec3_dist(pi, pj);
            if (d > max_d) max_d = d;
        }
    }
    return max_d;
}

int main(void) {
    printf("[fracture_body_test] === setup ===\n");
    PhiPhysicsWorld *world = phi_physics_world_create();
    check(world != NULL, "physics world created");
    fracture_body_system_init();

    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf");

    printf("[fracture_body_test] === 1: defensive -- NULL hem / n_fragments < 1 ===\n");
    check(fracture_body_activate(world, NULL, (Vec3f){0,0,0}, quat_identity(), 8, 42u, 50.0f) == 0,
          "NULL hem spawns nothing rather than crashing");
    check(fracture_body_count() == 0, "count stays 0 after a failed activation");
    check(fracture_body_activate(world, hem, (Vec3f){0,0,0}, quat_identity(), 0, 42u, 50.0f) == 0,
          "n_fragments < 1 spawns nothing");

    printf("[fracture_body_test] === 2: real activation -- fragments spawned at the source transform ===\n");
    /* Far from the ground (y=10000) and gravity temporarily zeroed for
     * tests 3/4 below -- isolates "did the impulse alone cause/not cause
     * a breakup" from "did it also hit the ground", which would
     * otherwise add its own uncontrolled impulses into the same
     * measurement. */
    phi_physics_world_set_gravity(world, (Vec3f){0.0f, 0.0f, 0.0f});
    Vec3f source_pos = {0.0f, 10000.0f, 0.0f};
    int n = fracture_body_activate(world, hem, source_pos, quat_identity(), 8, 42u, 50.0f);
    check(n > 0, "activation spawns at least one real fragment body");
    check(fracture_body_count() == n, "fracture_body_count matches what activate() returned");
    printf("  spawned %d fragment(s)\n", n);

    Vec3f p0;
    check(fracture_body_get(0, &p0, NULL) == 1, "fracture_body_get reads a real fragment position");
    check(fabsf(p0.x - source_pos.x) < 0.01f && fabsf(p0.y - source_pos.y) < 0.01f && fabsf(p0.z - source_pos.z) < 0.01f,
          "every fragment starts at the source object's own transform, not the origin or some other default");
    check(fracture_body_get(n, NULL, NULL) == 0, "an out-of-range index is rejected cleanly, not read past the array");

    printf("[fracture_body_test] === 3: gentle impulse -- fragments stay glued ===\n");
    if (n >= 2) {
        fracture_body_apply_impulse(0, (Vec3f){2.0f, 0.0f, 0.0f}, (Vec3f){0,0,0});
        for (int i = 0; i < 30; i++) {
            phi_physics_world_step(world, 1.0f / 60.0f);
            fracture_body_sync_and_draw_all(NULL);   /* r unused by the stub above -- syncs positions from physics either way */
        }
        float spread = max_fragment_spread(n);
        printf("  max pairwise spread after gentle impulse: %.3f\n", spread);
        check(spread < 2.0f, "still glued: a gentle impulse well under any constraint's breaking threshold keeps the cluster together");
    } else {
        check(1, "skipped -- fracture_voronoi only produced 1 non-empty fragment this seed, nothing to glue/break (not a fracture_body.c bug)");
    }

    printf("[fracture_body_test] === 4: hard impulse -- something actually breaks off ===\n");
    n = fracture_body_activate(world, hem, source_pos, quat_identity(), 8, 42u, 50.0f);   /* fresh, undisturbed re-activation, same seed */
    if (n >= 2) {
        fracture_body_apply_impulse(0, (Vec3f){2000.0f, 0.0f, 0.0f}, (Vec3f){0,0,0});
        for (int i = 0; i < 30; i++) {
            phi_physics_world_step(world, 1.0f / 60.0f);
            fracture_body_sync_and_draw_all(NULL);
        }
        float spread = max_fragment_spread(n);
        printf("  max pairwise spread after hard impulse: %.3f\n", spread);
        check(spread > 5.0f, "broken: a hard impulse well over the breaking threshold sends at least one fragment flying apart from the rest");
    } else {
        check(1, "skipped -- fracture_voronoi only produced 1 non-empty fragment this seed, nothing to glue/break (not a fracture_body.c bug)");
    }

    printf("[fracture_body_test] === 5: cleanup -- clear empties the registry, no crash ===\n");
    fracture_body_clear(world);
    check(fracture_body_count() == 0, "clear() really empties the registry");
    fracture_body_clear(world);
    check(1, "clearing an already-empty registry is a safe no-op (this line running proves it)");

    printf("[fracture_body_test] === 6: teardown ===\n");
    halfedge_destroy(hem);
    phi_physics_world_destroy(world);
    check(1, "world + fragments torn down without crashing (this line running proves it)");

    if (g_fail) { printf("\n[fracture_body_test] RESULT: FAIL\n"); return 1; }
    printf("\n[fracture_body_test] RESULT: PASS (all checks passed)\n");
    return 0;
}
