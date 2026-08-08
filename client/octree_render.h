#pragma once
#include "octree.h"

/* Vertex layout (interleaved, 28 bytes):
 *   pos:    3 x float  (12 bytes)
 *   normal: 3 x float  (12 bytes)
 *   mat_id: 1 x float  ( 4 bytes) — used as material/color index in shader
 */
#define VERTEX_STRIDE 7  /* floats per vertex */
#define VERTS_PER_QUAD 6 /* 2 tris */

typedef struct {
    float  *data;       /* interleaved vertex data */
    int     count;      /* number of vertices */
    int     capacity;   /* allocated vertices */
    unsigned int vbo;   /* GL VBO handle */
    int     dirty;      /* needs re-upload */
} RenderMesh;

RenderMesh *mesh_create(void);
void        mesh_destroy(RenderMesh *m);
void        mesh_rebuild(RenderMesh *m, const Octree *ot);
void        mesh_upload(RenderMesh *m);   /* uploads to GPU */
void        mesh_draw(RenderMesh *m);     /* glDrawArrays */
