#include "font.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif

/* SDF bake parameters — the exact values stb_truetype's own header comment
 * gives as its worked example (padding=5, onedge_value=180,
 * pixel_dist_scale=180/5.0=36.0), not invented here. padding is extra
 * border around each glyph's tight bounding box that still carries a real
 * (non-zero) distance value, which is what lets the fragment shader
 * antialias/outline right up to and slightly past the glyph's edge instead
 * of hitting a hard texture-clamp boundary. */
#define SDF_PADDING          5
#define SDF_ONEDGE_VALUE     180
#define SDF_PIXEL_DIST_SCALE 36.0f
/* The pixel height glyphs are actually rasterized at for the SDF bake —
 * independent of what size text is later DRAWN at (that's the whole point
 * of SDF: bake once, draw crisp at any scale). Baking a bit larger than
 * likely on-screen sizes gives the SDF more source resolution to work
 * from, at some atlas-memory cost — 48px is a reasonable middle ground for
 * a UI that mostly shows body/label text in the teens-to-20s pixel range. */
#define SDF_BAKE_PIXEL_HEIGHT 48.0f

static unsigned char *read_file(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *out_size = size;
    return buf;
}

/* Shelf packing (below, inline in font_load): glyphs are placed left-to-
 * right, wrapping to a new row when the current one is full; row height is
 * the tallest glyph placed in it so far. Not bin-packing-optimal, but for
 * ~95 ASCII SDF glyphs at a fixed bake size this wastes at most a few
 * percent of atlas area — not worth a real packer for this glyph count. */
typedef struct {
    int atlas_w, atlas_h;
    int cursor_x, cursor_y, row_h;
} Shelf;

Font *font_load(const char *ttf_path, float pixel_height) {
    long ttf_size;
    unsigned char *ttf_data = read_file(ttf_path, &ttf_size);
    if (!ttf_data) {
        printf("[font] failed to read '%s'\n", ttf_path);
        return NULL;
    }

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        printf("[font] failed to parse '%s'\n", ttf_path);
        free(ttf_data);
        return NULL;
    }

    float scale = stbtt_ScaleForPixelHeight(&info, SDF_BAKE_PIXEL_HEIGHT);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);

    /* First pass: generate every glyph's SDF bitmap and record its size,
     * before deciding the atlas dimensions (need every glyph's size to
     * pack them, and packing needs a target width first) — 512 wide is
     * generous enough that 95 glyphs at ~48px bake height fit in well
     * under 512 tall too (verified empirically below via the actual
     * cursor_y after packing, not just assumed). */
    unsigned char *bitmaps[FONT_NUM_CHARS] = {0};
    int bmp_w[FONT_NUM_CHARS] = {0}, bmp_h[FONT_NUM_CHARS] = {0};
    int bmp_xoff[FONT_NUM_CHARS] = {0}, bmp_yoff[FONT_NUM_CHARS] = {0};
    int advance[FONT_NUM_CHARS] = {0};

    for (int i = 0; i < FONT_NUM_CHARS; i++) {
        int codepoint = FONT_FIRST_CHAR + i;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, codepoint, &adv, &lsb);
        advance[i] = adv;
        int w, h, xoff, yoff;
        unsigned char *bmp = stbtt_GetCodepointSDF(&info, scale, codepoint,
                                                    SDF_PADDING, SDF_ONEDGE_VALUE, SDF_PIXEL_DIST_SCALE,
                                                    &w, &h, &xoff, &yoff);
        bitmaps[i] = bmp;  /* NULL for empty glyphs (space, etc.) -- handled below */
        bmp_w[i] = w; bmp_h[i] = h; bmp_xoff[i] = xoff; bmp_yoff[i] = yoff;
    }

    Shelf shelf = {512, 0, 0, 0, 0};
    int placed_x[FONT_NUM_CHARS], placed_y[FONT_NUM_CHARS];
    for (int i = 0; i < FONT_NUM_CHARS; i++) {
        if (!bitmaps[i]) { placed_x[i] = placed_y[i] = 0; continue; }
        if (shelf.cursor_x + bmp_w[i] > shelf.atlas_w) {
            shelf.cursor_x = 0;
            shelf.cursor_y += shelf.row_h;
            shelf.row_h = 0;
        }
        placed_x[i] = shelf.cursor_x;
        placed_y[i] = shelf.cursor_y;
        shelf.cursor_x += bmp_w[i];
        if (bmp_h[i] > shelf.row_h) shelf.row_h = bmp_h[i];
    }
    int atlas_w = shelf.atlas_w;
    int atlas_h = shelf.cursor_y + shelf.row_h;
    if (atlas_h < 1) atlas_h = 1;

    unsigned char *atlas = (unsigned char *)calloc((size_t)atlas_w * (size_t)atlas_h, 1);
    for (int i = 0; i < FONT_NUM_CHARS; i++) {
        if (!bitmaps[i]) continue;
        for (int y = 0; y < bmp_h[i]; y++) {
            memcpy(atlas + (size_t)(placed_y[i] + y) * atlas_w + placed_x[i],
                   bitmaps[i] + (size_t)y * bmp_w[i], (size_t)bmp_w[i]);
        }
        stbtt_FreeSDF(bitmaps[i], NULL);
    }

    Font *f = (Font *)calloc(1, sizeof(Font));
    f->atlas_w = atlas_w;
    f->atlas_h = atlas_h;
    f->pixel_height = pixel_height;
    /* ascent/descent/line_gap from stb are in font units; scale to the
     * SDF bake's pixel space so callers scaling by pixel_height/BAKE get
     * consistent results with the glyph quads themselves. */
    f->ascent   = ascent  * scale;
    f->descent  = descent * scale;
    f->line_gap = line_gap * scale;

    for (int i = 0; i < FONT_NUM_CHARS; i++) {
        GlyphInfo *g = &f->glyphs[i];
        if (bitmaps[i]) {
            g->u0 = (float)placed_x[i] / (float)atlas_w;
            g->v0 = (float)placed_y[i] / (float)atlas_h;
            g->u1 = (float)(placed_x[i] + bmp_w[i]) / (float)atlas_w;
            g->v1 = (float)(placed_y[i] + bmp_h[i]) / (float)atlas_h;
            g->w = (float)bmp_w[i];
            g->h = (float)bmp_h[i];
            g->xoff = (float)bmp_xoff[i];
            g->yoff = (float)bmp_yoff[i];
        }
        g->xadvance = advance[i] * scale;
    }

    glGenTextures(1, &f->atlas_tex);
    glBindTexture(GL_TEXTURE_2D, f->atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    printf("[font] baked '%s': atlas %dx%d, %d glyphs, tex=%u\n",
           ttf_path, atlas_w, atlas_h, FONT_NUM_CHARS, f->atlas_tex);

    free(atlas);
    free(ttf_data);
    return f;
}

void font_destroy(Font *f) {
    if (!f) return;
    if (f->atlas_tex) glDeleteTextures(1, &f->atlas_tex);
    free(f);
}

float font_text_width(const Font *f, const char *text, float pixel_height) {
    float scale = pixel_height / SDF_BAKE_PIXEL_HEIGHT;
    float w = 0.0f;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int idx = (int)*p - FONT_FIRST_CHAR;
        if (idx < 0 || idx >= FONT_NUM_CHARS) continue;
        w += f->glyphs[idx].xadvance * scale;
    }
    return w;
}
