#include "octree_render.h"
#include <stdlib.h>
#include <string.h>

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

void mesh_upload(RenderMesh *m) {
    mesh_upload_stride(m, VERTEX_STRIDE);
}

void mesh_upload_stride(RenderMesh *m, int stride_floats) {
    if (!m->dirty) return;
    if (!m->vbo) glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (size_t)m->count * (size_t)stride_floats * sizeof(float),
                 m->data, GL_STATIC_DRAW);
    m->dirty = 0;
}

void mesh_draw(RenderMesh *m) {
    if (m->count == 0) return;
    /* Caller has already bound the VBO and set attrib pointers */
    glDrawArrays(GL_TRIANGLES, 0, m->count);
}
