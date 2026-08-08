#include "cmap.h"
#include "octree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Serialize ---- */
typedef struct { uint8_t *buf; size_t len; size_t cap; } ByteBuf;

static void bb_push(ByteBuf *b, uint8_t v) {
    if (b->len >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->buf = (uint8_t *)realloc(b->buf, b->cap);
    }
    b->buf[b->len++] = v;
}

static void bb_pushN(ByteBuf *b, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) bb_push(b, data[i]);
}

static void serialize_node(ByteBuf *b, const OctreeNode *node) {
    bb_push(b, (uint8_t)node->type);
    if (node->type == NODE_SOLID) {
        bb_pushN(b, node->faces, 6);
    } else if (node->type == NODE_DEFORMED) {
        bb_pushN(b, (const uint8_t *)node->corners, 8);
        bb_pushN(b, node->faces, 6);
    } else if (node->type == NODE_SUBDIVIDED) {
        for (int i = 0; i < 8; i++) {
            if (node->children[i])
                serialize_node(b, node->children[i]);
            else {
                /* Missing child = EMPTY */
                bb_push(b, (uint8_t)NODE_EMPTY);
            }
        }
    }
    /* EMPTY: just the type byte */
}

uint8_t *cmap_serialize(const Octree *ot, size_t *out_size) {
    ByteBuf b = {0};
    /* Header */
    bb_pushN(&b, (const uint8_t *)"CMAP", 4);
    uint16_t ver = 2;
    bb_pushN(&b, (const uint8_t *)&ver, 2);
    bb_push(&b, 10);  /* world_size = log2(1024) */
    for (int i = 0; i < 25; i++) bb_push(&b, 0);   /* reserved */
    /* Nodes */
    serialize_node(&b, ot->root);
    *out_size = b.len;
    return b.buf;
}

int cmap_save(const Octree *ot, const char *path) {
    size_t size;
    uint8_t *buf = cmap_serialize(ot, &size);
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);
    return 0;
}

/* ---- Deserialize ---- */
typedef struct { const uint8_t *buf; size_t pos; size_t len; } ReadBuf;

static uint8_t rb_u8(ReadBuf *r) {
    return (r->pos < r->len) ? r->buf[r->pos++] : 0;
}

static OctreeNode *deserialize_node(ReadBuf *r) {
    OctreeNode *node = node_alloc();
    node->type = (NodeType)rb_u8(r);
    if (node->type == NODE_SOLID) {
        for (int i = 0; i < 6; i++) node->faces[i] = rb_u8(r);
    } else if (node->type == NODE_DEFORMED) {
        for (int i = 0; i < 8; i++) node->corners[i] = (int8_t)rb_u8(r);
        for (int i = 0; i < 6; i++) node->faces[i]   = rb_u8(r);
    } else if (node->type == NODE_SUBDIVIDED) {
        for (int i = 0; i < 8; i++)
            node->children[i] = deserialize_node(r);
    }
    return node;
}

Octree *cmap_deserialize(const uint8_t *buf, size_t size) {
    if (size < 32) return NULL;
    if (memcmp(buf, "CMAP", 4) != 0) return NULL;
    uint16_t ver; memcpy(&ver, buf + 4, 2);
    if (ver != 2) {
        printf("[cmap] unsupported .cmap version %u (expected 2)\n", (unsigned)ver);
        return NULL;
    }

    node_pool_reset();
    Octree *ot = (Octree *)malloc(sizeof(Octree));
    ot->depth      = MAX_OCTREE_DEPTH;
    ot->world_size = WORLD_SIZE;

    ReadBuf r = { buf, 32, size };  /* skip 32-byte header */
    ot->root = deserialize_node(&r);
    return ot;
}

Octree *cmap_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    Octree *ot = cmap_deserialize(buf, (size_t)sz);
    free(buf);
    return ot;
}
