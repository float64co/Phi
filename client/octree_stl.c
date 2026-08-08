#include "octree_stl.h"
#include "octree.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ---- Write helpers (little-endian) ---- */
static uint8_t *write_f32(uint8_t *p, float v) {
    memcpy(p, &v, 4); return p + 4;
}
static uint8_t *write_u32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4); return p + 4;
}
static uint8_t *write_u16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2); return p + 2;
}

/* ---- Triangle collection ---- */
typedef struct {
    float nx, ny, nz;
    float v[3][3];
} STLTri;

typedef struct {
    STLTri *tris;
    int     count;
    int     cap;
} TriBuf;

static void tribuf_push(TriBuf *tb, STLTri t) {
    if (tb->count >= tb->cap) {
        tb->cap = tb->cap ? tb->cap * 2 : (1 << 16);
        tb->tris = (STLTri *)realloc(tb->tris, tb->cap * sizeof(STLTri));
    }
    tb->tris[tb->count++] = t;
}

static void vec3_cross(float *out, const float *a, const float *b) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

static void vec3_normalize(float *v) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) { v[0]/=len; v[1]/=len; v[2]/=len; }
}

static float vec3_dist2(float ax, float ay, float az,
                         float bx, float by, float bz) {
    float dx=bx-ax, dy=by-ay, dz=bz-az;
    return dx*dx + dy*dy + dz*dz;
}

/* Push a quad as 2 triangles, splitting on shorter diagonal (Sauerbraten rule)
 * Quad vertices: a, b, c, d (in order around the face) */
static void push_quad(TriBuf *tb,
                       float ax, float ay, float az,
                       float bx, float by, float bz,
                       float cx, float cy, float cz,
                       float dx, float dy, float dz) {
    STLTri t1, t2;
    float edge1[3], edge2[3], norm[3];

    /* Choose diagonal: ac vs bd */
    float d_ac = vec3_dist2(ax,ay,az, cx,cy,cz);
    float d_bd = vec3_dist2(bx,by,bz, dx,dy,dz);

    if (d_ac <= d_bd) {
        /* Split: (a,b,c) and (a,c,d) */
        t1.v[0][0]=ax; t1.v[0][1]=ay; t1.v[0][2]=az;
        t1.v[1][0]=bx; t1.v[1][1]=by; t1.v[1][2]=bz;
        t1.v[2][0]=cx; t1.v[2][1]=cy; t1.v[2][2]=cz;
        t2.v[0][0]=ax; t2.v[0][1]=ay; t2.v[0][2]=az;
        t2.v[1][0]=cx; t2.v[1][1]=cy; t2.v[1][2]=cz;
        t2.v[2][0]=dx; t2.v[2][1]=dy; t2.v[2][2]=dz;
    } else {
        /* Split: (a,b,d) and (b,c,d) */
        t1.v[0][0]=ax; t1.v[0][1]=ay; t1.v[0][2]=az;
        t1.v[1][0]=bx; t1.v[1][1]=by; t1.v[1][2]=bz;
        t1.v[2][0]=dx; t1.v[2][1]=dy; t1.v[2][2]=dz;
        t2.v[0][0]=bx; t2.v[0][1]=by; t2.v[0][2]=bz;
        t2.v[1][0]=cx; t2.v[1][1]=cy; t2.v[1][2]=cz;
        t2.v[2][0]=dx; t2.v[2][1]=dy; t2.v[2][2]=dz;
    }

    /* Compute normals */
    for (STLTri *t = &t1; t <= &t2; t++) {
        edge1[0]=t->v[1][0]-t->v[0][0]; edge1[1]=t->v[1][1]-t->v[0][1]; edge1[2]=t->v[1][2]-t->v[0][2];
        edge2[0]=t->v[2][0]-t->v[0][0]; edge2[1]=t->v[2][1]-t->v[0][1]; edge2[2]=t->v[2][2]-t->v[0][2];
        vec3_cross(norm, edge1, edge2);
        vec3_normalize(norm);
        t->nx=norm[0]; t->ny=norm[1]; t->nz=norm[2];
    }
    tribuf_push(tb, t1);
    tribuf_push(tb, t2);
}

/* Walk octree and collect triangles */
static void collect_node(TriBuf *tb, const Octree *ot,
                           const OctreeNode *node,
                           int nx, int ny, int nz, int nsize) {
    if (node->type == NODE_EMPTY) return;
    if (node->type == NODE_SUBDIVIDED) {
        int half = nsize >> 1;
        for (int cz=0;cz<2;cz++)
        for (int cy=0;cy<2;cy++)
        for (int cx=0;cx<2;cx++) {
            int idx = child_idx(cx,cy,cz);
            if (node->children[idx])
                collect_node(tb, ot, node->children[idx],
                             nx+cx*half, ny+cy*half, nz+cz*half, half);
        }
        return;
    }

    float x0=(float)nx, y0=(float)ny, z0=(float)nz;
    float x1=x0+nsize,  y1=y0+nsize,  z1=z0+nsize;

    /* Apply corner deformation */
    float yb[4] = {y0,y0,y0,y0};  /* bottom: corners 0,1,2,3 */
    float yt[4] = {y1,y1,y1,y1};  /* top: corners 4,5,6,7 */
    if (node->type == NODE_DEFORMED) {
        float scale = nsize / 128.0f;
        for (int i=0;i<4;i++) { yb[i]+=node->corners[i]*scale; yt[i]+=node->corners[i+4]*scale; }
    }
    /* Corner positions:
     *  bot: 0=(x0,yb0,z0) 1=(x1,yb1,z0) 2=(x1,yb2,z1) 3=(x0,yb3,z1)
     *  top: 4=(x0,yt0,z0) 5=(x1,yt1,z0) 6=(x1,yt2,z1) 7=(x0,yt3,z1)
     */

    /* Only emit faces that border an empty/out-of-bounds neighbor */
    /* NEG_X (x=x0): 0,3,7,4 */
    if (!octree_is_solid(ot,nx-1,ny+nsize/2,nz+nsize/2))
        push_quad(tb, x0,yb[0],z0,  x0,yb[3],z1,  x0,yt[3],z1,  x0,yt[0],z0);

    /* POS_X (x=x1): 1,5,6,2 */
    if (!octree_is_solid(ot,nx+nsize,ny+nsize/2,nz+nsize/2))
        push_quad(tb, x1,yb[1],z0,  x1,yt[1],z0,  x1,yt[2],z1,  x1,yb[2],z1);

    /* NEG_Y (y bottom): 0,1,2,3 */
    if (!octree_is_solid(ot,nx+nsize/2,ny-1,nz+nsize/2))
        push_quad(tb, x0,yb[0],z0,  x1,yb[1],z0,  x1,yb[2],z1,  x0,yb[3],z1);

    /* POS_Y (y top): 4,7,6,5 */
    if (!octree_is_solid(ot,nx+nsize/2,ny+nsize,nz+nsize/2))
        push_quad(tb, x0,yt[0],z0,  x0,yt[3],z1,  x1,yt[2],z1,  x1,yt[1],z0);

    /* NEG_Z (z=z0): 0,4,5,1 */
    if (!octree_is_solid(ot,nx+nsize/2,ny+nsize/2,nz-1))
        push_quad(tb, x0,yb[0],z0,  x0,yt[0],z0,  x1,yt[1],z0,  x1,yb[1],z0);

    /* POS_Z (z=z1): 3,2,6,7 */
    if (!octree_is_solid(ot,nx+nsize/2,ny+nsize/2,nz+nsize))
        push_quad(tb, x0,yb[3],z1,  x1,yb[2],z1,  x1,yt[2],z1,  x0,yt[3],z1);
}

uint8_t *octree_export_stl(const Octree *ot, size_t *out_size) {
    TriBuf tb = {0};
    collect_node(&tb, ot, ot->root, 0, 0, 0, ot->world_size);

    size_t total = 80 + 4 + (size_t)tb.count * 50;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    uint8_t *p = buf;

    /* 80-byte header */
    const char *hdr = "Qek STL Export - Sauerbraten-style octree geometry";
    strncpy((char *)p, hdr, 80);
    p += 80;

    p = write_u32(p, (uint32_t)tb.count);

    for (int i = 0; i < tb.count; i++) {
        STLTri *t = &tb.tris[i];
        p = write_f32(p, t->nx);
        p = write_f32(p, t->ny);
        p = write_f32(p, t->nz);
        for (int v = 0; v < 3; v++) {
            p = write_f32(p, t->v[v][0]);
            p = write_f32(p, t->v[v][1]);
            p = write_f32(p, t->v[v][2]);
        }
        p = write_u16(p, 0);
    }

    free(tb.tris);
    *out_size = total;
    return buf;
}

void octree_stl_download(const Octree *ot) {
    size_t size = 0;
    uint8_t *buf = octree_export_stl(ot, &size);
    if (!buf) return;

#ifdef __EMSCRIPTEN__
    /* Pass buffer to JS as a Uint8Array, trigger download */
    EM_ASM({
        var buf = $0;
        var len = $1;
        var arr = new Uint8Array(Module.HEAPU8.buffer, buf, len);
        var copy = new Uint8Array(arr);  /* copy out before free */
        var blob = new Blob([copy], {type: 'application/octet-stream'});
        var url  = URL.createObjectURL(blob);
        var a    = document.createElement('a');
        a.href = url;
        a.download = 'map.stl';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }, buf, (int)size);
#else
    /* Fallback: write to file */
    FILE *f = fopen("map.stl", "wb");
    if (f) { fwrite(buf, 1, size, f); fclose(f); }
    printf("Exported %zu bytes to map.stl (%d triangles)\n", size, (int)((size-84)/50));
#endif
    free(buf);
}
