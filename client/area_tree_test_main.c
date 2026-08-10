/* Standalone, GL-free test harness for area_tree.c (Blender-style area
 * border resize/split/join) -- same "prove the subsystem works in
 * isolation" precedent as mesh_edit_test_main.c/fracture_test_main.c,
 * applied to the Native UI System's area tree instead of mesh topology.
 * Manually lays out a small tree (mirroring ui.c's own layout_area
 * formula: SPLIT_H's split fraction is a fraction of *width*, SPLIT_V of
 * *height*) rather than linking ui.c itself, since layout_area is static
 * and entangled with the rest of ui.c's GL-heavy state. */
#include "area_tree.h"
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

/* Mirrors ui.c's layout_area exactly (SPLIT_H divides width at x + w*split,
 * SPLIT_V divides height at y + h*split) -- duplicated here deliberately
 * rather than exposed from ui.c, since it's a handful of lines and pulling
 * ui.c's actual layout_area out would mean exposing GL-adjacent state for
 * no real benefit; this test cares that area_tree.c's OWN border-position
 * math (identical formula, see area_tree_find_border_hit) agrees with
 * whatever laid the tree out, not that it reuses the same function. */
static void layout(Area *a, float x, float y, float w, float h) {
    a->x = x; a->y = y; a->w = w; a->h = h;
    if (a->kind == AREA_SPLIT_H) {
        float w0 = w * a->split;
        layout(a->child[0], x, y, w0, h);
        layout(a->child[1], x + w0, y, w - w0, h);
    } else if (a->kind == AREA_SPLIT_V) {
        float h0 = h * a->split;
        layout(a->child[0], x, y, w, h0);
        layout(a->child[1], x, y + h0, w, h - h0);
    }
}

int main(void) {
    printf("[area_tree_test] === 1: split turns a leaf into a SPLIT_H with two leaf children ===\n");
    Area root = {0};
    root.kind = AREA_LEAF;
    root.panel_type = PANEL_SCENE;
    area_tree_split(&root, AREA_SPLIT_H);
    check(root.kind == AREA_SPLIT_H, "root is now AREA_SPLIT_H");
    check(root.split == 0.5f, "default split fraction is 0.5");
    check(root.child[0] != NULL && root.child[1] != NULL, "both children allocated");
    check(root.child[0]->kind == AREA_LEAF && root.child[1]->kind == AREA_LEAF, "both children are leaves");
    check(root.child[0]->panel_type == PANEL_SCENE && root.child[1]->panel_type == PANEL_SCENE,
          "both children inherit the original panel type (Blender's own split default)");

    printf("[area_tree_test] === 2: splitting an already-split area is a no-op ===\n");
    Area *saved_c0 = root.child[0];
    area_tree_split(&root, AREA_SPLIT_V);
    check(root.kind == AREA_SPLIT_H && root.child[0] == saved_c0, "no change -- area_tree_split only acts on leaves");

    printf("[area_tree_test] === 3: find_parent ===\n");
    check(area_tree_find_parent(&root, root.child[0]) == &root, "child[0]'s parent is root");
    check(area_tree_find_parent(&root, root.child[1]) == &root, "child[1]'s parent is root");
    check(area_tree_find_parent(&root, &root) == NULL, "root itself has no parent");

    printf("[area_tree_test] === 4: border hit-testing against a laid-out tree ===\n");
    layout(&root, 0.0f, 0.0f, 800.0f, 600.0f);   /* SPLIT_H, split=0.5 -> border at x=400 */
    check(area_tree_find_border_hit(&root, 400, 300) == &root, "exact border position hits");
    check(area_tree_find_border_hit(&root, 402, 300) == &root, "within AREA_BORDER_HIT_PX still hits");
    check(area_tree_find_border_hit(&root, 400, -10) == NULL, "outside the split's own y-range misses, even at the right x");
    check(area_tree_find_border_hit(&root, 450, 300) == NULL, "far from the border misses");
    check(area_tree_find_border_hit(root.child[0], 400, 300) == NULL, "a leaf has no border of its own to hit");

    printf("[area_tree_test] === 4b: find_leaf_at (hover routing, e.g. mouse-wheel scroll target) ===\n");
    check(area_tree_find_leaf_at(&root, 100, 300) == root.child[0], "a point in the left half finds the left leaf");
    check(area_tree_find_leaf_at(&root, 700, 300) == root.child[1], "a point in the right half finds the right leaf");
    check(area_tree_find_leaf_at(&root, 400, 300) == root.child[1], "the exact border x belongs to the right leaf (half-open [x, x+w) ranges)");
    check(area_tree_find_leaf_at(&root, -10, 300) == NULL, "outside the tree's own bounds (negative x) misses entirely");
    check(area_tree_find_leaf_at(&root, 100, 700) == NULL, "outside the tree's own bounds (y past the bottom) misses entirely");
    check(area_tree_find_leaf_at(NULL, 100, 300) == NULL, "a NULL root is a safe miss, not a crash");

    printf("[area_tree_test] === 5: resize-to-mouse, including clamping ===\n");
    area_tree_resize_to_mouse(&root, 200, 300);
    check(root.split == 0.25f, "split recomputed from mouse x / area width (200/800 = 0.25)");
    area_tree_resize_to_mouse(&root, 10, 300);
    check(root.split == AREA_SPLIT_MIN, "clamps to AREA_SPLIT_MIN rather than collapsing a side to ~0");
    area_tree_resize_to_mouse(&root, 790, 300);
    check(root.split == AREA_SPLIT_MAX, "clamps to AREA_SPLIT_MAX rather than collapsing the other side");
    root.split = 0.5f; layout(&root, 0.0f, 0.0f, 800.0f, 600.0f);   /* restore for the tests below */

    printf("[area_tree_test] === 6: nested split (SPLIT_V inside one SPLIT_H child) ===\n");
    Area *right = root.child[1];
    area_tree_split(right, AREA_SPLIT_V);
    layout(&root, 0.0f, 0.0f, 800.0f, 600.0f);
    check(right->kind == AREA_SPLIT_V, "right child is now its own SPLIT_V");
    check(area_tree_find_border_hit(&root, 500, 300) == right, "a click inside the nested split's region finds the NESTED border, not the outer one");
    check(area_tree_find_border_hit(&root, 400, 300) == &root, "a click on the OUTER border still finds root, not the nested split");
    check(area_tree_find_leaf_at(&root, 500, 100) == right->child[0], "find_leaf_at recurses into a nested split correctly (top half)");
    check(area_tree_find_leaf_at(&root, 500, 500) == right->child[1], "find_leaf_at recurses into a nested split correctly (bottom half)");

    printf("[area_tree_test] === 7: join collapses a split back into a single leaf ===\n");
    Area *right_top = right->child[0];
    right_top->panel_type = PANEL_OUTLINER;
    area_tree_join_with_sibling(&root, right_top);
    check(right->kind == AREA_LEAF, "the collapsed parent (formerly SPLIT_V) is now a leaf");
    check(right->panel_type == PANEL_OUTLINER, "the leaf carries the panel type of whichever side was right-clicked to join");
    check(right->child[0] == NULL && right->child[1] == NULL, "child pointers cleared");

    printf("[area_tree_test] === 8: joining the root (no parent) is a safe no-op ===\n");
    Area lone_root = {0};
    lone_root.kind = AREA_LEAF;
    lone_root.panel_type = PANEL_CONSOLE;
    area_tree_join_with_sibling(&lone_root, &lone_root);
    check(lone_root.kind == AREA_LEAF && lone_root.panel_type == PANEL_CONSOLE,
          "no-op -- a root with no parent can't be joined, and nothing crashes trying");

    printf("[area_tree_test] === 9: area_tree_free on a real multi-level tree and on NULL ===\n");
    area_tree_free(root.child[0]);
    area_tree_free(root.child[1]);
    area_tree_free(NULL);   /* must not crash */
    check(1, "freed a real subtree and a NULL pointer without crashing (leak-checked via ASan build, see Makefile)");

    if (g_fail) printf("\n[area_tree_test] RESULT: FAIL\n");
    else printf("\n[area_tree_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
