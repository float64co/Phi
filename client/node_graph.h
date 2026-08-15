#pragma once

/* Node graphs -- Phase 6's C-owned graph topology (see phi.md's "Geometry
 * and Animation Nodes"). This file owns nodes/links/positions and knows
 * nothing about MicroPython or Python values -- mirrors halfedge.c/
 * scene_objects.c/light.c's own "pure C data model, no interpreter
 * dependency" split, so it's standalone-testable and reusable by a future
 * visual editor widget (Phase 1's Native UI System, per phi.md's "Graph
 * ownership: C owns topology, Python drives and evaluates it") without
 * that widget needing to touch MicroPython either.
 *
 * A node's TYPE (its name, socket signature, and the Python function that
 * implements it) is a separate concept, owned by mp_port.c's own registry
 * (the @phi.node decorator, mirroring @phi.panel's s_panels) -- this file
 * only stores each node INSTANCE's type_name as a string plus whatever
 * literal params it was given; resolving that name to a callable and
 * actually evaluating the graph is mp_port.c's job (see phi_mp_graph_
 * evaluate), which is the one place in this system that legitimately
 * needs to hold live mp_obj_t values, and only transiently, for the
 * duration of one evaluate() call -- never stored here.
 *
 * Params are intentionally narrow: one float OR one string per named
 * param, not an arbitrary Python value. A socket wired to an upstream
 * node's output carries whatever real value that node's Python function
 * returned (a Mesh, a number, anything) -- that path goes through
 * mp_port.c's evaluator and never touches this file's param storage at
 * all. Params here only cover a node's UNCONNECTED inputs (e.g.
 * noise_displace's scale=0.3 when nothing feeds it), the same "simple
 * typed value" scope this codebase's own PhiProp system already commits
 * to for object/light properties -- not a general serialization format. */

#define PHI_GRAPH_MAX_GRAPHS  16
#define PHI_GRAPH_MAX_NODES   64
#define PHI_GRAPH_MAX_LINKS   128
#define PHI_GRAPH_MAX_PARAMS  8
#define PHI_GRAPH_NAME_LEN    32

typedef enum {
    PHI_GRAPH_KIND_GEOMETRY = 0,
    PHI_GRAPH_KIND_ANIMATION,
} PhiGraphKind;

/* Widened from an is_string bool after a real gap found in practice:
 * shader node params (principled_bsdf's base_color, emission's color)
 * are 3-tuples, and neither a float nor a string can hold one -- adding
 * a real third case here rather than routing around it (e.g. by forcing
 * every color into a string and parsing it back out, which would just
 * move the problem). */
typedef enum {
    PHI_GRAPH_PARAM_FLOAT = 0,
    PHI_GRAPH_PARAM_STRING = 1,
    PHI_GRAPH_PARAM_VEC3 = 2,
} PhiGraphParamType;

typedef struct {
    char              name[PHI_GRAPH_NAME_LEN];
    PhiGraphParamType type;
    float             value_f;
    char              value_s[64];
    float             value_vec3[3];
} PhiGraphParam;

typedef struct {
    int            used;
    char           type_name[PHI_GRAPH_NAME_LEN];
    PhiGraphParam  params[PHI_GRAPH_MAX_PARAMS];
    int            param_count;
    float          pos_x, pos_y;     /* editor layout only -- never read by the evaluator */
} PhiGraphNode;

typedef struct {
    int   used;
    int   src_node, dst_node;        /* indices into PhiGraph.nodes[] */
    char  src_socket[PHI_GRAPH_NAME_LEN];
    char  dst_socket[PHI_GRAPH_NAME_LEN];
} PhiGraphLink;

typedef struct {
    int           used;
    PhiGraphKind  kind;
    PhiGraphNode  nodes[PHI_GRAPH_MAX_NODES];
    int           node_count;        /* high-water mark -- append-only, same tombstone-free-by-construction model as scene_objects.c (nodes are never individually deleted this pass, only whole graphs) */
    PhiGraphLink  links[PHI_GRAPH_MAX_LINKS];
    int           link_count;
} PhiGraph;

/* Clears the registry -- call once at startup, same convention as
 * scene_objects_init/light_system_init. */
void phi_graph_system_init(void);

/* Returns a new graph's id (1-based, stable, 0 means invalid -- same
 * convention as PhiLight.id), or -1 if the registry is full. */
int phi_graph_create(PhiGraphKind kind);

/* Frees a graph's slot for reuse. No-op if graph_id doesn't exist. */
void phi_graph_destroy(int graph_id);

PhiGraph *phi_graph_find(int graph_id);

/* Adds a node of the given type (a bare name -- this file does not
 * validate it against mp_port.c's type registry, since it has no
 * visibility into that; an unknown type_name is only ever caught at
 * evaluate() time). Returns the new node's index within the graph
 * (0-based, stable -- nodes are append-only, see PhiGraph.node_count),
 * or -1 if the graph doesn't exist or is full. */
int phi_graph_add_node(int graph_id, const char *type_name);

/* Sets/overwrites a named param on a node (float or string -- whichever
 * is called last for a given name wins; a param can't be both). Returns
 * 1 on success, 0 if the graph/node doesn't exist or PHI_GRAPH_MAX_PARAMS
 * is already used by other names. */
int phi_graph_set_param_float(int graph_id, int node_idx, const char *name, float value);
int phi_graph_set_param_string(int graph_id, int node_idx, const char *name, const char *value);
int phi_graph_set_param_vec3(int graph_id, int node_idx, const char *name, const float value[3]);

void phi_graph_set_position(int graph_id, int node_idx, float x, float y);

/* Connects src_node's src_socket (an OUTPUT) to dst_node's dst_socket (an
 * INPUT). Multiple links into the same dst_socket are allowed at this
 * layer (the last one added simply wins at evaluate time, see mp_port.c)
 * -- rejecting that is a UI/authoring-time concern, not a topology
 * invariant this file needs to enforce. Returns 1 on success, 0 if the
 * graph/either node index doesn't exist or PHI_GRAPH_MAX_LINKS is full.
 * Does NOT check for cycles here -- see phi_graph_topological_order,
 * which is where a cycle is actually a well-defined failure (this keeps
 * connect() a cheap, always-fast operation during interactive editing,
 * matching how Blender/most node editors let you wire up a cycle and
 * only complain when it would actually need to evaluate). */
int phi_graph_connect(int graph_id, int src_node, const char *src_socket,
                       int dst_node, const char *dst_socket);

/* Kahn's algorithm over the link-induced dependency graph. Writes
 * out_order[0..N) (caller-provided, PHI_GRAPH_MAX_NODES capacity) with
 * node indices in a valid evaluation order (every node's dependencies
 * appear before it) and returns N (the number of LIVE, i.e. used, nodes
 * -- a node with no id -- .used==0 slot never exists in this append-only
 * model, so N always equals g->node_count). Returns -1 if the graph
 * contains a cycle, a real checked failure rather than an infinite loop
 * or silently-wrong partial order. */
int phi_graph_topological_order(const PhiGraph *g, int *out_order);
