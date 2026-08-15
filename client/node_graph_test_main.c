/* Standalone, no-GL/no-MicroPython test harness for node_graph.c's pure C
 * graph topology + Kahn's-algorithm topological sort -- same "prove the
 * data structure correct in isolation" spirit as mesh_edit_test_main.c/
 * fracture_test_main.c. Node TYPE resolution and actually calling Python
 * functions is mp_port.c's job (see mp_node_test_main.c for that half);
 * this file only proves the C-owned CRUD and evaluation ORDER are right. */
#include "node_graph.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

/* True iff `before` appears earlier in order[0..n) than `after` -- the
 * actual invariant a topological order must satisfy per dependency edge,
 * checked directly rather than comparing against one hardcoded "the"
 * order (Kahn's algorithm over a graph with parallel branches has more
 * than one valid answer -- B and C in a diamond can come in either
 * order, only A-before-both and both-before-D are real constraints). */
static int comes_before(const int *order, int n, int before, int after) {
    int pos_before = -1, pos_after = -1;
    for (int i = 0; i < n; i++) {
        if (order[i] == before) pos_before = i;
        if (order[i] == after) pos_after = i;
    }
    return pos_before >= 0 && pos_after >= 0 && pos_before < pos_after;
}

int main(void) {
    printf("[node_graph_test] === setup ===\n");
    phi_graph_system_init();

    printf("[node_graph_test] === 1: create/find/destroy a graph ===\n");
    int gid = phi_graph_create(PHI_GRAPH_KIND_GEOMETRY);
    CHECK(gid > 0, "phi_graph_create returns a real positive id");
    PhiGraph *g = phi_graph_find(gid);
    CHECK(g != NULL && g->kind == PHI_GRAPH_KIND_GEOMETRY, "phi_graph_find returns the real graph with the kind it was created with");
    CHECK(phi_graph_find(999999) == NULL, "phi_graph_find on a nonexistent id returns NULL");

    printf("[node_graph_test] === 2: add_node/set_param/set_position ===\n");
    int n0 = phi_graph_add_node(gid, "input_mesh");
    int n1 = phi_graph_add_node(gid, "noise_displace");
    CHECK(n0 == 0 && n1 == 1, "add_node returns stable, sequential 0-based indices");
    CHECK(g->node_count == 2, "the graph's real node_count reflects both adds");
    CHECK(strcmp(g->nodes[n1].type_name, "noise_displace") == 0, "the node's real type_name was stored");

    CHECK(phi_graph_set_param_float(gid, n1, "scale", 0.3f), "set_param_float on a real node succeeds");
    CHECK(g->nodes[n1].params[0].type == PHI_GRAPH_PARAM_FLOAT && g->nodes[n1].params[0].value_f == 0.3f, "the float param's real value was stored");
    CHECK(phi_graph_set_param_string(gid, n0, "asset", "rock.glb"), "set_param_string on a real node succeeds");
    CHECK(g->nodes[n0].params[0].type == PHI_GRAPH_PARAM_STRING && strcmp(g->nodes[n0].params[0].value_s, "rock.glb") == 0, "the string param's real value was stored");

    /* Overwriting an existing param name switches its type in place
     * rather than leaking a second slot under the same name. */
    CHECK(phi_graph_set_param_string(gid, n1, "scale", "big"), "re-setting an existing param name (now as a string) succeeds");
    CHECK(g->nodes[n1].param_count == 1, "overwriting 'scale' did NOT allocate a second param slot");
    CHECK(g->nodes[n1].params[0].type == PHI_GRAPH_PARAM_STRING && strcmp(g->nodes[n1].params[0].value_s, "big") == 0, "the overwritten param really is a string now");

    /* A third real type (see node_graph.h's own comment on why is_string
     * got widened to a real enum): re-setting the SAME name once more, now
     * as a vec3, proves overwrite-in-place works across all three types,
     * not just string<->string. */
    float color[3] = { 0.25f, 0.5f, 0.75f };
    CHECK(phi_graph_set_param_vec3(gid, n1, "scale", color), "set_param_vec3 on an existing param name (now as a vec3) succeeds");
    CHECK(g->nodes[n1].param_count == 1, "overwriting 'scale' a second time (string -> vec3) STILL didn't allocate a new slot");
    CHECK(g->nodes[n1].params[0].type == PHI_GRAPH_PARAM_VEC3 &&
          g->nodes[n1].params[0].value_vec3[0] == 0.25f && g->nodes[n1].params[0].value_vec3[1] == 0.5f && g->nodes[n1].params[0].value_vec3[2] == 0.75f,
          "the real vec3 value was stored exactly");

    phi_graph_set_position(gid, n1, 300.0f, 120.0f);
    CHECK(g->nodes[n1].pos_x == 300.0f && g->nodes[n1].pos_y == 120.0f, "set_position wrote real editor-layout coordinates");

    CHECK(phi_graph_add_node(999999, "x") == -1, "add_node on a nonexistent graph fails");
    CHECK(phi_graph_set_param_float(gid, 999, "x", 1.0f) == 0, "set_param_float on an out-of-range node index fails");

    printf("[node_graph_test] === 3: connect + a real diamond-shaped topological order ===\n");
    /*      A
     *     / \
     *    B   C
     *     \ /
     *      D
     * A must come before B and C; B and C must both come before D; B vs
     * C's relative order is NOT constrained (a real property of this
     * graph shape, checked as such below rather than assumed away). */
    int gid2 = phi_graph_create(PHI_GRAPH_KIND_GEOMETRY);
    int a = phi_graph_add_node(gid2, "A");
    int b = phi_graph_add_node(gid2, "B");
    int c = phi_graph_add_node(gid2, "C");
    int d = phi_graph_add_node(gid2, "D");
    CHECK(phi_graph_connect(gid2, a, "out", b, "in"), "connect A->B succeeds");
    CHECK(phi_graph_connect(gid2, a, "out", c, "in"), "connect A->C succeeds");
    CHECK(phi_graph_connect(gid2, b, "out", d, "in1"), "connect B->D succeeds");
    CHECK(phi_graph_connect(gid2, c, "out", d, "in2"), "connect C->D succeeds");
    CHECK(phi_graph_connect(gid2, a, "out", 999, "in") == 0, "connect with an out-of-range dst node index fails");

    int order[PHI_GRAPH_MAX_NODES];
    PhiGraph *g2 = phi_graph_find(gid2);
    int n = phi_graph_topological_order(g2, order);
    CHECK(n == 4, "topological_order returns all 4 live nodes, not a partial count");
    CHECK(comes_before(order, n, a, b), "A really comes before B in the returned order");
    CHECK(comes_before(order, n, a, c), "A really comes before C");
    CHECK(comes_before(order, n, b, d), "B really comes before D");
    CHECK(comes_before(order, n, c, d), "C really comes before D");

    printf("[node_graph_test] === 4: a real cycle is detected, not infinite-looped or silently ordered ===\n");
    int gid3 = phi_graph_create(PHI_GRAPH_KIND_GEOMETRY);
    int x = phi_graph_add_node(gid3, "X");
    int y = phi_graph_add_node(gid3, "Y");
    int z = phi_graph_add_node(gid3, "Z");
    phi_graph_connect(gid3, x, "out", y, "in");
    phi_graph_connect(gid3, y, "out", z, "in");
    phi_graph_connect(gid3, z, "out", x, "in");   /* closes the cycle X->Y->Z->X */
    PhiGraph *g3 = phi_graph_find(gid3);
    int cyc = phi_graph_topological_order(g3, order);
    CHECK(cyc == -1, "a real 3-node cycle is reported as -1, not a wrong/partial order");

    printf("[node_graph_test] === 5: a cycle among only SOME nodes doesn't break ordering of the rest ===\n");
    int gid4 = phi_graph_create(PHI_GRAPH_KIND_GEOMETRY);
    int p = phi_graph_add_node(gid4, "P");     /* independent, no edges at all */
    int q = phi_graph_add_node(gid4, "Q");
    int r = phi_graph_add_node(gid4, "R");
    phi_graph_connect(gid4, q, "out", r, "in");
    phi_graph_connect(gid4, r, "out", q, "in");   /* Q<->R cycle, independent of P */
    PhiGraph *g4 = phi_graph_find(gid4);
    int cyc2 = phi_graph_topological_order(g4, order);
    CHECK(cyc2 == -1, "the Q<->R cycle is still detected even though P has no edges at all");
    (void)p;

    printf("[node_graph_test] === 6: destroy frees a slot for real reuse ===\n");
    int before_destroy_count = 0;
    for (int i = 1; i <= PHI_GRAPH_MAX_GRAPHS; i++) if (phi_graph_find(i)) before_destroy_count++;
    phi_graph_destroy(gid4);
    CHECK(phi_graph_find(gid4) == NULL, "the destroyed graph is genuinely gone");
    int gid5 = phi_graph_create(PHI_GRAPH_KIND_ANIMATION);
    CHECK(gid5 == gid4, "creating a new graph after a destroy reuses the freed slot (same id back), not silently growing forever");
    CHECK(phi_graph_find(gid5)->kind == PHI_GRAPH_KIND_ANIMATION, "the reused slot's stale GEOMETRY state is really gone -- fresh kind, not leaked from the destroyed graph");
    CHECK(phi_graph_find(gid5)->node_count == 0, "the reused slot's stale node_count is really gone");

    printf("[node_graph_test] === 7: registry capacity is a real, checked bound ===\n");
    int already_alive = 0;
    for (int i = 1; i <= PHI_GRAPH_MAX_GRAPHS; i++) if (phi_graph_find(i)) already_alive++;
    int newly_created = 0, first_failure_seen = 0, saw_failure_after_full = 0;
    for (int i = 0; i < PHI_GRAPH_MAX_GRAPHS + 4; i++) {
        int r = phi_graph_create(PHI_GRAPH_KIND_GEOMETRY);
        if (r > 0) newly_created++;
        else { first_failure_seen = 1; saw_failure_after_full = 1; }
    }
    CHECK(already_alive + newly_created == PHI_GRAPH_MAX_GRAPHS, "exactly PHI_GRAPH_MAX_GRAPHS graphs can exist at once (already-alive + newly-created fills the registry exactly)");
    CHECK(first_failure_seen && saw_failure_after_full, "once full, further creates fail (-1) rather than overflowing the registry");

    printf("\n[node_graph_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
