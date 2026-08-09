#pragma once
#include "ui.h"   /* Area, AreaKind, PanelType */

/* Pure Area-tree structural operations (hit-testing a border, resizing,
 * splitting, joining) -- deliberately split out of ui.c, which is GL/
 * rendering-heavy, so this logic has zero GL dependency and can be unit-
 * tested standalone (see area_tree_test_main.c) the same way mesh_edit.c/
 * fracture.c's own topology mutations are tested independent of the
 * renderer. ui.c owns the interactive/visual side (drag state, hover
 * highlighting, menus) and calls into these; this module only ever
 * touches Area's plain data fields (x/y/w/h/split/kind/child/panel_type),
 * never g_ui or anything GL-related.
 *
 * All structural edits mutate a target Area IN PLACE (leaf <-> split)
 * rather than splicing a new node into its parent: Area has no parent
 * pointer (nothing in this codebase needed one before this), so in-place
 * mutation means every existing raw Area* anywhere -- a UI's root
 * included -- stays valid across a split/join with no pointer fixup
 * needed anywhere. */

#define AREA_BORDER_HIT_PX 5.0f
#define AREA_SPLIT_MIN     0.1f
#define AREA_SPLIT_MAX     0.9f

/* Finds the SPLIT area whose divider (x,y) is within AREA_BORDER_HIT_PX
 * of, searching the whole tree rooted at `a`. Checks each node's own
 * border before recursing into its children, since a nested split's
 * border always lives strictly inside one child's region and can never
 * overlap its parent's own. Returns NULL if (x,y) isn't near any border. */
Area *area_tree_find_border_hit(Area *a, int x, int y);

/* Recomputes a SPLIT area's split fraction from a live mouse position,
 * clamped to [AREA_SPLIT_MIN, AREA_SPLIT_MAX] so neither side ever
 * collapses to nothing. No-op if `a` isn't currently a SPLIT (e.g. it was
 * joined into a leaf by something else mid-drag). */
void area_tree_resize_to_mouse(Area *a, int mouse_x, int mouse_y);

/* Finds `target`'s parent within the tree rooted at `root`, or NULL if
 * `target` IS `root` (no parent) or isn't in the tree at all. */
Area *area_tree_find_parent(Area *root, Area *target);

/* Splits leaf `a` into two leaves of the same panel type (a freshly split
 * area starts as a copy of what you split, matching Blender's own
 * default), turning `a` itself into the new SPLIT_H/SPLIT_V node. No-op
 * if `a` isn't currently a leaf. */
void area_tree_split(Area *a, AreaKind direction);

/* Joins leaf `leaf` with its sibling by collapsing their shared parent
 * split back into a single leaf carrying `leaf`'s own panel type --
 * frees both old children first. No-op if `leaf` has no parent (it IS
 * `root`, the whole layout is already just one area). */
void area_tree_join_with_sibling(Area *root, Area *leaf);

/* Frees an Area and, if it's a SPLIT, both its children recursively.
 * Safe to call with NULL. */
void area_tree_free(Area *a);
