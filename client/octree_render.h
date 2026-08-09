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
void        mesh_upload(RenderMesh *m);   /* uploads to GPU, assumes VERTEX_STRIDE */
/* Same as mesh_upload but for a buffer whose vertex layout isn't the
 * generic VERTEX_STRIDE=7 format -- MeshObject's own richer PBR layout
 * (see meshobject.h's MESHOBJ_VERTEX_STRIDE) uses this instead, since
 * uploading its 14-float vertices through the hardcoded-stride mesh_upload
 * would only copy roughly half of each vertex's actual bytes to the GPU. */
void        mesh_upload_stride(RenderMesh *m, int stride_floats);
void        mesh_draw(RenderMesh *m);     /* glDrawArrays */
