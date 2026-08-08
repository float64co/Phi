#include "halfedge.h"
#include <stdlib.h>
#include <string.h>

static void *grow(void *p, int *cap, int need, size_t elem_size) {
    if (need <= *cap) return p;
    int new_cap = *cap ? *cap * 2 : 8;
    while (new_cap < need) new_cap *= 2;
    p = realloc(p, (size_t)new_cap * elem_size);
    *cap = new_cap;
    return p;
}

HalfEdgeMesh *halfedge_create(void) {
    return (HalfEdgeMesh *)calloc(1, sizeof(HalfEdgeMesh));
}

void halfedge_destroy(HalfEdgeMesh *hem) {
    if (!hem) return;
    free(hem->verts);
    free(hem->edges);
    free(hem->faces);
    free(hem);
}

int halfedge_add_vertex(HalfEdgeMesh *hem, float x, float y, float z) {
    hem->verts = (HEVertex *)grow(hem->verts, &hem->vert_cap, hem->vert_count + 1, sizeof(HEVertex));
    HEVertex *v = &hem->verts[hem->vert_count];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    v->edge = -1;
    return hem->vert_count++;
}

/* Linear scan for an existing half-edge running from `from` to `to` — used
 * to find the just-added edge going the OPPOSITE direction of a new one,
 * i.e. its twin. See halfedge.h's struct comment on why this is O(n) here
 * rather than hash-mapped. */
static int find_edge(const HalfEdgeMesh *hem, int from, int to) {
    for (int e = 0; e < hem->edge_count; e++) {
        if (hem->edges[e].origin == from && hem->edges[hem->edges[e].next].origin == to)
            return e;
    }
    return -1;
}

int halfedge_add_face(HalfEdgeMesh *hem, const int *vert_indices, int n) {
    hem->faces = (HEFace *)grow(hem->faces, &hem->face_cap, hem->face_count + 1, sizeof(HEFace));
    int face_idx = hem->face_count++;

    hem->edges = (HEEdge *)grow(hem->edges, &hem->edge_cap, hem->edge_count + n, sizeof(HEEdge));
    int first_edge = hem->edge_count;
    for (int i = 0; i < n; i++) {
        HEEdge *e = &hem->edges[hem->edge_count + i];
        e->origin = vert_indices[i];
        e->face   = face_idx;
        e->twin   = -1;
        e->next   = first_edge + (i + 1) % n;
        e->prev   = first_edge + (i - 1 + n) % n;
    }
    hem->edge_count += n;

    for (int i = 0; i < n; i++) {
        int edge_idx = first_edge + i;
        int a = vert_indices[i], b = vert_indices[(i + 1) % n];
        hem->verts[a].edge = edge_idx;
        /* Twin lookup: an existing edge running b->a is this edge's
         * opposite. Search only edges added before this face's own (the
         * ones just added above can't be a twin of each other within the
         * same face for a valid, non-self-adjacent mesh). */
        int twin = find_edge(hem, b, a);
        if (twin >= 0 && twin < first_edge) {
            hem->edges[edge_idx].twin = twin;
            hem->edges[twin].twin = edge_idx;
        }
    }

    hem->faces[face_idx].edge = first_edge;
    hem->faces[face_idx].count = n;
    return face_idx;
}

HalfEdgeMesh *halfedge_build_from_triangles(const float *positions, int pos_count,
                                             const unsigned short *indices, int index_count) {
    HalfEdgeMesh *hem = halfedge_create();
    for (int i = 0; i < pos_count; i++)
        halfedge_add_vertex(hem, positions[i*3+0], positions[i*3+1], positions[i*3+2]);
    for (int i = 0; i + 2 < index_count; i += 3) {
        int tri[3] = { indices[i], indices[i+1], indices[i+2] };
        halfedge_add_face(hem, tri, 3);
    }
    return hem;
}

void halfedge_face_verts(const HalfEdgeMesh *hem, int f, int *out_verts) {
    const HEFace *face = &hem->faces[f];
    int e = face->edge;
    for (int i = 0; i < face->count; i++) {
        out_verts[i] = hem->edges[e].origin;
        e = hem->edges[e].next;
    }
}

void halfedge_flatten_triangles(const HalfEdgeMesh *hem,
                                 float **out_positions, int *out_pos_count,
                                 unsigned short **out_indices, int *out_index_count) {
    *out_pos_count = hem->vert_count;
    *out_positions = (float *)malloc(sizeof(float) * 3 * (size_t)hem->vert_count);
    for (int i = 0; i < hem->vert_count; i++) {
        (*out_positions)[i*3+0] = hem->verts[i].pos[0];
        (*out_positions)[i*3+1] = hem->verts[i].pos[1];
        (*out_positions)[i*3+2] = hem->verts[i].pos[2];
    }

    /* Every face is a triangle in this pass (see halfedge.h) — 3 indices
     * per face, no fan-triangulation of n-gons needed yet. */
    *out_index_count = hem->face_count * 3;
    *out_indices = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)*out_index_count);
    for (int f = 0; f < hem->face_count; f++) {
        int verts[3];
        halfedge_face_verts(hem, f, verts);
        (*out_indices)[f*3+0] = (unsigned short)verts[0];
        (*out_indices)[f*3+1] = (unsigned short)verts[1];
        (*out_indices)[f*3+2] = (unsigned short)verts[2];
    }
}
