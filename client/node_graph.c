#include "node_graph.h"
#include <string.h>

static PhiGraph s_graphs[PHI_GRAPH_MAX_GRAPHS];

void phi_graph_system_init(void) {
    memset(s_graphs, 0, sizeof(s_graphs));
}

int phi_graph_create(PhiGraphKind kind) {
    for (int i = 0; i < PHI_GRAPH_MAX_GRAPHS; i++) {
        if (!s_graphs[i].used) {
            memset(&s_graphs[i], 0, sizeof(PhiGraph));
            s_graphs[i].used = 1;
            s_graphs[i].kind = kind;
            return i + 1;   /* 1-based id, same convention as PhiLight.id */
        }
    }
    return -1;
}

void phi_graph_destroy(int graph_id) {
    PhiGraph *g = phi_graph_find(graph_id);
    if (g) memset(g, 0, sizeof(PhiGraph));   /* used=0 falls out of the zeroed struct */
}

PhiGraph *phi_graph_find(int graph_id) {
    int idx = graph_id - 1;
    if (idx < 0 || idx >= PHI_GRAPH_MAX_GRAPHS) return NULL;
    if (!s_graphs[idx].used) return NULL;
    return &s_graphs[idx];
}

static void copy_name(char *dst, size_t dst_cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n > dst_cap - 1) n = dst_cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

int phi_graph_add_node(int graph_id, const char *type_name) {
    PhiGraph *g = phi_graph_find(graph_id);
    if (!g || g->node_count >= PHI_GRAPH_MAX_NODES) return -1;
    int idx = g->node_count++;
    PhiGraphNode *n = &g->nodes[idx];
    memset(n, 0, sizeof(PhiGraphNode));
    n->used = 1;
    copy_name(n->type_name, sizeof(n->type_name), type_name);
    return idx;
}

static PhiGraphNode *find_node(PhiGraph *g, int node_idx) {
    if (!g || node_idx < 0 || node_idx >= g->node_count) return NULL;
    if (!g->nodes[node_idx].used) return NULL;
    return &g->nodes[node_idx];
}

/* Shared by set_param_float/set_param_string -- finds an existing param
 * slot with this name (to overwrite, whichever type it's set as next) or
 * the first free slot. Returns NULL if the node is out of param slots
 * AND has no existing slot with this name. */
static PhiGraphParam *find_or_alloc_param(PhiGraphNode *n, const char *name) {
    for (int i = 0; i < n->param_count; i++) {
        if (strcmp(n->params[i].name, name) == 0) return &n->params[i];
    }
    if (n->param_count >= PHI_GRAPH_MAX_PARAMS) return NULL;
    PhiGraphParam *p = &n->params[n->param_count++];
    memset(p, 0, sizeof(PhiGraphParam));
    copy_name(p->name, sizeof(p->name), name);
    return p;
}

/* Shared by set_param_float/set_param_string below -- both need the
 * identical graph-id -> node -> param-slot resolution and only differ in
 * which field they write once they have it. */
static PhiGraphParam *resolve_param_slot(int graph_id, int node_idx, const char *name) {
    PhiGraph *g = phi_graph_find(graph_id);
    PhiGraphNode *n = find_node(g, node_idx);
    if (!n) return NULL;
    return find_or_alloc_param(n, name);
}

int phi_graph_set_param_float(int graph_id, int node_idx, const char *name, float value) {
    PhiGraphParam *p = resolve_param_slot(graph_id, node_idx, name);
    if (!p) return 0;
    p->type = PHI_GRAPH_PARAM_FLOAT;
    p->value_f = value;
    return 1;
}

int phi_graph_set_param_string(int graph_id, int node_idx, const char *name, const char *value) {
    PhiGraphParam *p = resolve_param_slot(graph_id, node_idx, name);
    if (!p) return 0;
    p->type = PHI_GRAPH_PARAM_STRING;
    copy_name(p->value_s, sizeof(p->value_s), value);
    return 1;
}

int phi_graph_set_param_vec3(int graph_id, int node_idx, const char *name, const float value[3]) {
    PhiGraphParam *p = resolve_param_slot(graph_id, node_idx, name);
    if (!p) return 0;
    p->type = PHI_GRAPH_PARAM_VEC3;
    p->value_vec3[0] = value[0];
    p->value_vec3[1] = value[1];
    p->value_vec3[2] = value[2];
    return 1;
}

void phi_graph_set_position(int graph_id, int node_idx, float x, float y) {
    PhiGraph *g = phi_graph_find(graph_id);
    PhiGraphNode *n = find_node(g, node_idx);
    if (!n) return;
    n->pos_x = x;
    n->pos_y = y;
}

int phi_graph_connect(int graph_id, int src_node, const char *src_socket,
                       int dst_node, const char *dst_socket) {
    PhiGraph *g = phi_graph_find(graph_id);
    if (!g) return 0;
    if (!find_node(g, src_node) || !find_node(g, dst_node)) return 0;
    if (g->link_count >= PHI_GRAPH_MAX_LINKS) return 0;
    PhiGraphLink *l = &g->links[g->link_count++];
    l->used = 1;
    l->src_node = src_node;
    l->dst_node = dst_node;
    copy_name(l->src_socket, sizeof(l->src_socket), src_socket);
    copy_name(l->dst_socket, sizeof(l->dst_socket), dst_socket);
    return 1;
}

int phi_graph_topological_order(const PhiGraph *g, int *out_order) {
    if (!g) return -1;
    int n = g->node_count;
    int indegree[PHI_GRAPH_MAX_NODES];
    int done[PHI_GRAPH_MAX_NODES];
    for (int i = 0; i < n; i++) { indegree[i] = 0; done[i] = 0; }
    for (int l = 0; l < g->link_count; l++) {
        if (!g->links[l].used) continue;
        int dst = g->links[l].dst_node;
        if (dst >= 0 && dst < n) indegree[dst]++;
    }

    int order_count = 0;
    for (int pass = 0; pass < n; pass++) {
        int picked = -1;
        for (int i = 0; i < n; i++) {
            if (!g->nodes[i].used || done[i]) continue;
            if (indegree[i] == 0) { picked = i; break; }
        }
        if (picked < 0) break;   /* no zero-indegree node left among the unprocessed -- cycle */
        out_order[order_count++] = picked;
        done[picked] = 1;
        for (int l = 0; l < g->link_count; l++) {
            if (!g->links[l].used) continue;
            if (g->links[l].src_node == picked) {
                int dst = g->links[l].dst_node;
                if (dst >= 0 && dst < n && !done[dst]) indegree[dst]--;
            }
        }
    }
    if (order_count < n) return -1;   /* cycle among the remaining nodes */
    return order_count;
}
