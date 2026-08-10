#include "area_tree.h"
#include <stdlib.h>

Area *area_tree_find_border_hit(Area *a, int x, int y) {
    if (a->kind == AREA_LEAF) return NULL;
    if (a->kind == AREA_SPLIT_H) {
        float border_x = a->x + a->w * a->split;
        if ((float)x >= border_x - AREA_BORDER_HIT_PX && (float)x <= border_x + AREA_BORDER_HIT_PX &&
            (float)y >= a->y && (float)y <= a->y + a->h) {
            return a;
        }
    } else {
        float border_y = a->y + a->h * a->split;
        if ((float)y >= border_y - AREA_BORDER_HIT_PX && (float)y <= border_y + AREA_BORDER_HIT_PX &&
            (float)x >= a->x && (float)x <= a->x + a->w) {
            return a;
        }
    }
    Area *found = area_tree_find_border_hit(a->child[0], x, y);
    return found ? found : area_tree_find_border_hit(a->child[1], x, y);
}

Area *area_tree_find_leaf_at(Area *a, int x, int y) {
    if (!a) return NULL;
    if ((float)x < a->x || (float)x >= a->x + a->w || (float)y < a->y || (float)y >= a->y + a->h)
        return NULL;
    if (a->kind == AREA_LEAF) return a;
    Area *found = area_tree_find_leaf_at(a->child[0], x, y);
    return found ? found : area_tree_find_leaf_at(a->child[1], x, y);
}

void area_tree_resize_to_mouse(Area *a, int mouse_x, int mouse_y) {
    if (a->kind == AREA_SPLIT_H) {
        if (a->w < 1.0f) return;
        float s = ((float)mouse_x - a->x) / a->w;
        a->split = s < AREA_SPLIT_MIN ? AREA_SPLIT_MIN : (s > AREA_SPLIT_MAX ? AREA_SPLIT_MAX : s);
    } else if (a->kind == AREA_SPLIT_V) {
        if (a->h < 1.0f) return;
        float s = ((float)mouse_y - a->y) / a->h;
        a->split = s < AREA_SPLIT_MIN ? AREA_SPLIT_MIN : (s > AREA_SPLIT_MAX ? AREA_SPLIT_MAX : s);
    }
}

Area *area_tree_find_parent(Area *root, Area *target) {
    if (root->kind == AREA_LEAF) return NULL;
    if (root->child[0] == target || root->child[1] == target) return root;
    Area *found = area_tree_find_parent(root->child[0], target);
    return found ? found : area_tree_find_parent(root->child[1], target);
}

void area_tree_free(Area *a) {
    if (!a) return;
    if (a->kind != AREA_LEAF) {
        area_tree_free(a->child[0]);
        area_tree_free(a->child[1]);
    }
    free(a);
}

void area_tree_split(Area *a, AreaKind direction) {
    if (a->kind != AREA_LEAF) return;
    PanelType old_type = a->panel_type;
    Area *c0 = (Area *)calloc(1, sizeof(Area));
    c0->kind = AREA_LEAF; c0->panel_type = old_type;
    Area *c1 = (Area *)calloc(1, sizeof(Area));
    c1->kind = AREA_LEAF; c1->panel_type = old_type;
    a->kind = direction;
    a->split = 0.5f;
    a->child[0] = c0;
    a->child[1] = c1;
    a->type_menu_open = 0;
}

void area_tree_join_with_sibling(Area *root, Area *leaf) {
    Area *parent = area_tree_find_parent(root, leaf);
    if (!parent) return;
    PanelType keep_type = leaf->panel_type;
    area_tree_free(parent->child[0]);
    area_tree_free(parent->child[1]);
    parent->kind = AREA_LEAF;
    parent->panel_type = keep_type;
    parent->child[0] = parent->child[1] = NULL;
    parent->type_menu_open = 0;
}
