#include "octree_render.h"
#include "octree.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <GLES2/gl2.h>
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif

RenderMesh *mesh_create(void) {
    RenderMesh *m = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    m->capacity = 1 << 18;  /* ~262k vertices initial */
    m->data = (float *)malloc(m->capacity * VERTEX_STRIDE * sizeof(float));
    return m;
}

void mesh_destroy(RenderMesh *m) {
    free(m->data);
    free(m);
}

static void mesh_push_vertex(RenderMesh *m,
                              float px, float py, float pz,
                              float nx, float ny, float nz,
                              float mat) {
    if (m->count >= m->capacity) {
        m->capacity *= 2;
        m->data = (float *)realloc(m->data, m->capacity * VERTEX_STRIDE * sizeof(float));
    }
    float *v = m->data + m->count * VERTEX_STRIDE;
    v[0]=px; v[1]=py; v[2]=pz;
    v[3]=nx; v[4]=ny; v[5]=nz;
    v[6]=mat;
    m->count++;
}

/* Push a quad as two triangles (CCW winding = front face) */
static void mesh_push_quad(RenderMesh *m,
                             float ax, float ay, float az,
                             float bx, float by, float bz,
                             float cx, float cy, float cz,
                             float dx, float dy, float dz,
                             float nx, float ny, float nz,
                             float mat) {
    /* tri 1: a, b, c */
    mesh_push_vertex(m, ax,ay,az, nx,ny,nz, mat);
    mesh_push_vertex(m, bx,by,bz, nx,ny,nz, mat);
    mesh_push_vertex(m, cx,cy,cz, nx,ny,nz, mat);
    /* tri 2: a, c, d */
    mesh_push_vertex(m, ax,ay,az, nx,ny,nz, mat);
    mesh_push_vertex(m, cx,cy,cz, nx,ny,nz, mat);
    mesh_push_vertex(m, dx,dy,dz, nx,ny,nz, mat);
}

/* Check if a neighbor voxel in given direction is solid (face culling) */
static int neighbor_solid(const Octree *ot,
                            int nx, int ny, int nz, int nsize,
                            int face) {
    int dx=0, dy=0, dz=0;
    switch(face) {
        case FACE_NEG_X: dx=-1; break;
        case FACE_POS_X: dx= 1; break;
        case FACE_NEG_Y: dy=-1; break;
        case FACE_POS_Y: dy= 1; break;
        case FACE_NEG_Z: dz=-1; break;
        case FACE_POS_Z: dz= 1; break;
    }
    /* Sample center of neighbor */
    int sx = nx + dx * nsize + (dx==0 ? nsize/2 : (dx>0 ? 1 : -1));
    int sy = ny + dy * nsize + (dy==0 ? nsize/2 : (dy>0 ? 1 : -1));
    int sz = nz + dz * nsize + (dz==0 ? nsize/2 : (dz>0 ? 1 : -1));
    return octree_is_solid(ot, sx, sy, sz);
}

static void emit_node_faces(RenderMesh *m, const Octree *ot,
                              const OctreeNode *node,
                              int nx, int ny, int nz, int nsize) {
    if (node->type == NODE_EMPTY) return;
    if (node->type == NODE_SUBDIVIDED) {
        int half = nsize >> 1;
        for (int cz = 0; cz < 2; cz++)
        for (int cy = 0; cy < 2; cy++)
        for (int cx = 0; cx < 2; cx++) {
            int idx = child_idx(cx, cy, cz);
            if (node->children[idx])
                emit_node_faces(m, ot, node->children[idx],
                                nx+cx*half, ny+cy*half, nz+cz*half, half);
        }
        return;
    }
    /* SOLID or DEFORMED: emit visible faces */
    float x0=(float)nx, y0=(float)ny, z0=(float)nz;
    float x1=x0+nsize,  y1=y0+nsize,  z1=z0+nsize;

    /* Corner deformation for DEFORMED nodes */
    float cy[8] = {y0,y0,y0,y0,y1,y1,y1,y1};
    if (node->type == NODE_DEFORMED) {
        float scale = nsize / 128.0f;
        cy[0] += node->corners[0] * scale;
        cy[1] += node->corners[1] * scale;
        cy[2] += node->corners[2] * scale;
        cy[3] += node->corners[3] * scale;
        cy[4] += node->corners[4] * scale;
        cy[5] += node->corners[5] * scale;
        cy[6] += node->corners[6] * scale;
        cy[7] += node->corners[7] * scale;
    }

    /* Bottom corners: 0=x0z0, 1=x1z0, 2=x1z1, 3=x0z1 */
    /* Top corners:    4=x0z0, 5=x1z0, 6=x1z1, 7=x0z1 */

    /* NEG_X face (x=x0): corners 0,3,7,4 (bottom-front, bottom-back, top-back, top-front) */
    if (!neighbor_solid(ot, nx, ny, nz, nsize, FACE_NEG_X))
        mesh_push_quad(m,
            x0,cy[0],z0,  x0,cy[3],z1,  x0,cy[7],z1,  x0,cy[4],z0,
            -1,0,0, (float)node->faces[FACE_NEG_X]);

    /* POS_X face (x=x1): corners 1,5,6,2 */
    if (!neighbor_solid(ot, nx, ny, nz, nsize, FACE_POS_X))
        mesh_push_quad(m,
            x1,cy[1],z0,  x1,cy[5],z0,  x1,cy[6],z1,  x1,cy[2],z1,
            1,0,0, (float)node->faces[FACE_POS_X]);

    /* NEG_Y face (y=y0): corners 0,1,2,3 */
    if (!neighbor_solid(ot, nx, ny, nz, nsize, FACE_NEG_Y))
        mesh_push_quad(m,
            x0,cy[0],z0,  x1,cy[1],z0,  x1,cy[2],z1,  x0,cy[3],z1,
            0,-1,0, (float)node->faces[FACE_NEG_Y]);

    /* POS_Y face (y=y1): corners 4,7,6,5 */
    if (!neighbor_solid(ot, nx, ny, nz, nsize, FACE_POS_Y))
        mesh_push_quad(m,
            x0,cy[4],z0,  x0,cy[7],z1,  x1,cy[6],z1,  x1,cy[5],z0,
            0,1,0, (float)node->faces[FACE_POS_Y]);

    /* NEG_Z face (z=z0): corners 0,4,5,1 */
    if (!neighbor_solid(ot, nx, ny, nz, nsize, FACE_NEG_Z))
        mesh_push_quad(m,
            x0,cy[0],z0,  x0,cy[4],z0,  x1,cy[5],z0,  x1,cy[1],z0,
            0,0,-1, (float)node->faces[FACE_NEG_Z]);

    /* POS_Z face (z=z1): corners 3,2,6,7 */
    if (!neighbor_solid(ot, nx, ny, nz, nsize, FACE_POS_Z))
        mesh_push_quad(m,
            x0,cy[3],z1,  x1,cy[2],z1,  x1,cy[6],z1,  x0,cy[7],z1,
            0,0,1, (float)node->faces[FACE_POS_Z]);
}

void mesh_rebuild(RenderMesh *m, const Octree *ot) {
    m->count = 0;
    emit_node_faces(m, ot, ot->root, 0, 0, 0, ot->world_size);
    m->dirty = 1;
}

void mesh_upload(RenderMesh *m) {
    if (!m->dirty) return;
    if (!m->vbo) glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 m->count * VERTEX_STRIDE * sizeof(float),
                 m->data, GL_STATIC_DRAW);
    m->dirty = 0;
}

void mesh_draw(RenderMesh *m) {
    if (m->count == 0) return;
    /* Caller (renderer_draw_world) has already bound the VBO and set attrib pointers */
    glDrawArrays(GL_TRIANGLES, 0, m->count);
}
