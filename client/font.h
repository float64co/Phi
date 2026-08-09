#pragma once

/* SDF font rendering — Phase 1 Native UI System foundation. Bakes a
 * signed-distance-field glyph atlas from a TTF at load time (via
 * stb_truetype's built-in stbtt_GetCodepointSDF — not hand-rolled), so
 * text stays crisp at any on-screen scale with a single bake, matching
 * phi.md's original "SDF fonts, one atlas baked at startup, crisp at any
 * size" spec directly (not a plain-bitmap compromise).
 *
 * ASCII 32-126 only for this first pass (covers every panel/menu/chat
 * string this UI needs) — no Unicode range selection yet, easy to extend
 * later without changing the atlas format.
 */

#define FONT_FIRST_CHAR 32
#define FONT_NUM_CHARS  95  /* 32..126 inclusive */

typedef struct {
    float u0, v0, u1, v1;   /* atlas UV rect, 0..1 */
    float w, h;              /* glyph quad size in pixels, at the baked pixel_height */
    float xoff, yoff;        /* offset from the pen position to the quad's top-left */
    float xadvance;          /* how far to move the pen after this glyph */
} GlyphInfo;

typedef struct {
    unsigned int atlas_tex;  /* GL_R8: SDF distance value per texel */
    int          atlas_w, atlas_h;
    float        pixel_height;   /* the size this atlas was baked at */
    float        ascent, descent, line_gap;  /* in the same pixel scale as pixel_height */
    GlyphInfo    glyphs[FONT_NUM_CHARS];
} Font;

/* Loads `ttf_path` and bakes an SDF atlas at `pixel_height` (the font size
 * text drawn with this Font will look best at, though the SDF technique
 * degrades gracefully at other scales — that's the point). Returns NULL on
 * failure (bad path, corrupt/unparseable font). */
Font *font_load(const char *ttf_path, float pixel_height);
void  font_destroy(Font *f);

/* Width of `text` in pixels if drawn at `pixel_height` (which may differ
 * from the atlas's own baked size — scales the glyph metrics). Needed for
 * layout (centering, wrapping, cursor positioning) without actually
 * drawing anything. */
float font_text_width(const Font *f, const char *text, float pixel_height);
