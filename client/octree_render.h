#pragma once
#include "vec3.h"   /* Vec3f -- local_bmin/local_bmax below */

/* Generic flat-triangle-buffer render mesh (interleaved vertex data + a
 * GL VBO handle) -- previously also carried mesh_rebuild(), which
 * flattened the Qek octree voxel world into one of these; that function
 * (and this header's octree.h include) is gone along with the rest of
 * Qek's world/gameplay code (see phi.md's Phase 1 status). What remains
 * here is genuinely octree-independent: MeshObject's own vertex buffers
 * (meshobject.c) are built and uploaded through this same RenderMesh/
 * mesh_create/mesh_upload_stride machinery. */

/* Vertex layout (interleaved, 28 bytes):
 *   pos:    3 x float  (12 bytes)
 *   normal: 3 x float  (12 bytes)
 *   mat_id: 1 x float  ( 4 bytes) — used as material/color index in shader
 */
#define VERTEX_STRIDE 7  /* floats per vertex */
#define VERTS_PER_QUAD 6 /* 2 tris */

/* One texture/material draw range within a RenderMesh's own vertex buffer
 * -- [start, start+count) is a glDrawArrays range, `texture` a real GL
 * texture name (0 = none, flat per-vertex base_color only, see HEFace::
 * texture's own comment). Lets renderer_draw_mesh_object issue one draw
 * call per texture group instead of one for a whole multi-material glTF
 * import (see meshobject_build_render_mesh_from_halfedge, which is what
 * actually populates RenderMesh::batches -- computed alongside the flat
 * vertex buffer it already builds, not a separate pass over hem). Only
 * MeshObject's PBR path uses this; the generic VERTEX_STRIDE=7 users
 * (world/ground/players/rockets) just leave batch_count at 0. */
typedef struct {
    int start;
    int count;
    unsigned int texture;
} MeshBatch;

#define MESHOBJECT_MAX_BATCHES 64

typedef struct {
    float  *data;       /* interleaved vertex data */
    int     count;      /* number of vertices */
    int     capacity;   /* allocated vertices */
    unsigned int vbo;   /* GL VBO handle */
    /* Cached per-mesh VAO -- glGenVertexArrays'd and configured (attrib
     * pointers bound to vbo) once, on first draw, by whichever
     * renderer_draw_* call actually knows this mesh's vertex layout (see
     * renderer_draw_mesh_object) -- octree_render.c/mesh_upload_stride
     * doesn't build it itself since it's layout-agnostic (stride is just
     * a parameter here, not a fixed format). 0 = not yet built. Safe to
     * keep across re-uploads: glVertexAttribPointer's binding is to the
     * BUFFER OBJECT (vbo), not its current contents, so a later
     * mesh_upload_stride's glBufferData re-specifying that same vbo's
     * data store never invalidates this VAO's attrib setup -- only a
     * genuinely different vbo handle would (never happens here, vbo is
     * glGenBuffers'd once and reused, see mesh_upload_stride). */
    unsigned int vao;
    int     dirty;      /* needs re-upload */
    MeshBatch batches[MESHOBJECT_MAX_BATCHES];
    int       batch_count;   /* 0 = no batches -- caller draws [0,count) as one untextured range */

    /* Local-space AABB of this mesh's actual vertex data, for frustum
     * culling (frustum.h's aabb_world_bounds) -- populated by
     * meshobject_build_render_mesh_from_halfedge (a real bounds scan,
     * not the "assumes centered on local origin" half-extents shortcut
     * meshobject_local_aabb_half_extents uses for physics box shapes),
     * alongside the vertex loop it already runs, so this costs no extra
     * pass. has_bounds is 0 for a RenderMesh nothing has populated this
     * for yet (e.g. gizmo.c/fracture_body.c's own generic-stride content)
     * -- a real, honest "no bounds available, caller must treat as always
     * visible" signal, not stale/zeroed min==max data that would look
     * like a degenerate real box and wrongly cull. */
    float local_bmin[3], local_bmax[3];
    int   has_bounds;
} RenderMesh;

RenderMesh *mesh_create(void);
void        mesh_destroy(RenderMesh *m);
void        mesh_upload(RenderMesh *m);   /* uploads to GPU, assumes VERTEX_STRIDE */
/* Same as mesh_upload but for a buffer whose vertex layout isn't the
 * generic VERTEX_STRIDE=7 format -- MeshObject's own richer PBR layout
 * (see meshobject.h's MESHOBJ_VERTEX_STRIDE) uses this instead, since
 * uploading its 14-float vertices through the hardcoded-stride mesh_upload
 * would only copy roughly half of each vertex's actual bytes to the GPU. */
void        mesh_upload_stride(RenderMesh *m, int stride_floats);
void        mesh_draw(RenderMesh *m);     /* glDrawArrays */
