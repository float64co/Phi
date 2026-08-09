#include "ui.h"
#include "meshobject.h"
#include "renderer.h"
#include "gbuffer.h"
#include "octree_render.h"
#include "net.h"
#include "console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif

#ifdef __EMSCRIPTEN__
#define UI_SHADER_HEADER "#version 300 es\nprecision mediump float;\n"
#else
#define UI_SHADER_HEADER "#version 330 core\n"
#endif

/* ---- Shaders: one shared vertex shape (pixel-space pos + uv), three tiny
 * fragment variants (flat color / SDF text / icon alpha mask) — matching
 * this codebase's established preference (gbuffer.c) for several small
 * single-purpose shaders over one branchy uber-shader. ---- */
static const char *UI_VERT_SRC =
    UI_SHADER_HEADER
    "layout(location=0) in vec2 a_pos;\n"   /* pixel space, origin top-left, y-down */
    "layout(location=1) in vec2 a_uv;\n"
    "uniform vec2 u_screen_size;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x / u_screen_size.x * 2.0 - 1.0,\n"
    "                   1.0 - a_pos.y / u_screen_size.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char *UI_RECT_FRAG_SRC =
    UI_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform vec4 u_color;\n"
    "out vec4 out_color;\n"
    "void main() { out_color = u_color; }\n";

/* Standard SDF text technique: sample the distance field, threshold around
 * the bake's onedge value (0.5 once the atlas is normalized to [0,1] by
 * the R8 texture format itself — stb_truetype's onedge_value=180/255 maps
 * to ~0.706, not exactly 0.5, so the threshold below matches that rather
 * than assuming 0.5), antialiased over a screen-space-derivative-sized
 * band via smoothstep — the widely-used approach for SDF text rendering
 * (Valve's 2007 SDF paper popularized it), not invented here. */
static const char *UI_SDF_FRAG_SRC =
    UI_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  float dist = texture(u_tex, v_uv).r;\n"
    "  float edge = 180.0 / 255.0;\n"
    "  float aa = fwidth(dist) * 1.5 + 0.001;\n"
    "  float alpha = smoothstep(edge - aa, edge + aa, dist);\n"
    "  out_color = vec4(u_color.rgb, u_color.a * alpha);\n"
    "}\n";

static const char *UI_ICON_FRAG_SRC =
    UI_SHADER_HEADER
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "  float a = texture(u_tex, v_uv).a;\n"
    "  out_color = vec4(u_color.rgb, u_color.a * a);\n"
    "}\n";

static unsigned int compile(GLenum type, const char *src) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); printf("[ui] shader compile error: %s\n", log); }
    return s;
}
static unsigned int link(const char *vsrc, const char *fsrc) {
    unsigned int vs = compile(GL_VERTEX_SHADER, vsrc), fs = compile(GL_FRAGMENT_SHADER, fsrc);
    unsigned int p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(p, 512, NULL, log); printf("[ui] program link error: %s\n", log); }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}
static void gl_check(const char *where) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) printf("[ui] GL error 0x%04x at %s\n", e, where);
}

/* ---- Global UI state ---- */
typedef struct {
    unsigned int rect_prog, sdf_prog, icon_prog;
    int rect_u_screen, rect_u_color;
    int sdf_u_screen, sdf_u_tex, sdf_u_color;
    int icon_u_screen, icon_u_tex, icon_u_color;
    unsigned int vao, vbo;
    int screen_w, screen_h;

    Font *font_body;    /* IBM Plex Sans Regular */
    Font *font_bold;    /* IBM Plex Sans Bold */
    Font *font_brand;   /* IBM Plex Sans BoldItalic -- branding bar mark */
    Font *font_mono;    /* IBM Plex Mono Regular -- console/chat */

    SvgIcon icon_scene, icon_outliner, icon_properties, icon_console, icon_chat;
    SvgIcon icon_node_editor, icon_curve_editor, icon_undo, icon_redo;

    Area *root;
    Area *panels[16];   /* flat list of every leaf, for hit-testing/iteration */
    int   panel_count;

    unsigned int selected_object_id;  /* 0xFFFFFFFF = none, matches gbuffer_pick_object_id's sentinel */

    /* 3D scene right-click context menu -- see ui_open_scene_context_menu() */
    int   ctx_menu_open;
    float ctx_menu_x, ctx_menu_y;
} UIState;

static UIState g_ui;

static const char *PANEL_NAMES[PANEL_TYPE_COUNT] = {
    "Scene", "Outliner", "Properties", "Console", "Chat", "Node Editor (not implemented yet)", "Curve Editor (not implemented yet)"
};

/* ---- Draw primitives ---- */
static void draw_quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                       unsigned int program, int u_screen, unsigned int tex, int u_tex, int u_color,
                       float r, float g, float b, float a) {
    float verts[6 * 4] = {
        x,   y,   u0, v0,
        x+w, y,   u1, v0,
        x+w, y+h, u1, v1,
        x,   y,   u0, v0,
        x+w, y+h, u1, v1,
        x,   y+h, u0, v1,
    };
    glUseProgram(program);
    glUniform2f(u_screen, (float)g_ui.screen_w, (float)g_ui.screen_h);
    glUniform4f(u_color, r, g, b, a);
    if (tex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); glUniform1i(u_tex, 0); }
    glBindVertexArray(g_ui.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_ui.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void ui_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_quad(x, y, w, h, 0, 0, 1, 1, g_ui.rect_prog, g_ui.rect_u_screen, 0, 0, g_ui.rect_u_color, r, g, b, a);
    glDisable(GL_BLEND);
}

static void ui_icon_draw(float x, float y, float size, SvgIcon icon, float r, float g, float b, float a) {
    if (!icon.texture) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_quad(x, y, size, size, 0, 0, 1, 1, g_ui.icon_prog, g_ui.icon_u_screen, icon.texture, g_ui.icon_u_tex, g_ui.icon_u_color, r, g, b, a);
    glDisable(GL_BLEND);
}

/* Draws `text` with its baseline at (x, y + font->ascent*scale) -- i.e.
 * (x,y) is the top-left of the text's em box, matching how ui_rect/other
 * layout code already thinks about positions, rather than requiring
 * callers to compute a baseline offset themselves. */
static float ui_text_draw(float x, float y, const char *text, const Font *font, float pixel_height,
                           float r, float g, float b, float a) {
    if (!font) return 0.0f;
    float scale = pixel_height / 48.0f;  /* SDF_BAKE_PIXEL_HEIGHT in font.c */
    float pen_x = x;
    float baseline = y + font->ascent * scale;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int idx = (int)*p - FONT_FIRST_CHAR;
        if (idx < 0 || idx >= FONT_NUM_CHARS) continue;
        const GlyphInfo *gl_ = &font->glyphs[idx];
        if (gl_->w > 0 && gl_->h > 0) {
            float gx = pen_x + gl_->xoff * scale;
            float gy = baseline + gl_->yoff * scale;
            draw_quad(gx, gy, gl_->w * scale, gl_->h * scale, gl_->u0, gl_->v0, gl_->u1, gl_->v1,
                      g_ui.sdf_prog, g_ui.sdf_u_screen, font->atlas_tex, g_ui.sdf_u_tex, g_ui.sdf_u_color, r, g, b, a);
        }
        pen_x += gl_->xadvance * scale;
    }
    glDisable(GL_BLEND);
    return pen_x - x;
}

/* ---- Init ---- */
int ui_init(void) {
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.selected_object_id = 0xFFFFFFFFu;

    g_ui.rect_prog = link(UI_VERT_SRC, UI_RECT_FRAG_SRC);
    g_ui.rect_u_screen = glGetUniformLocation(g_ui.rect_prog, "u_screen_size");
    g_ui.rect_u_color  = glGetUniformLocation(g_ui.rect_prog, "u_color");

    g_ui.sdf_prog = link(UI_VERT_SRC, UI_SDF_FRAG_SRC);
    g_ui.sdf_u_screen = glGetUniformLocation(g_ui.sdf_prog, "u_screen_size");
    g_ui.sdf_u_tex    = glGetUniformLocation(g_ui.sdf_prog, "u_tex");
    g_ui.sdf_u_color  = glGetUniformLocation(g_ui.sdf_prog, "u_color");

    g_ui.icon_prog = link(UI_VERT_SRC, UI_ICON_FRAG_SRC);
    g_ui.icon_u_screen = glGetUniformLocation(g_ui.icon_prog, "u_screen_size");
    g_ui.icon_u_tex    = glGetUniformLocation(g_ui.icon_prog, "u_tex");
    g_ui.icon_u_color  = glGetUniformLocation(g_ui.icon_prog, "u_color");

    glGenVertexArrays(1, &g_ui.vao);
    glBindVertexArray(g_ui.vao);
    glGenBuffers(1, &g_ui.vbo);

    g_ui.font_body  = font_load("assets/fonts/IBMPlexSans-Regular.ttf", 15.0f);
    g_ui.font_bold  = font_load("assets/fonts/IBMPlexSans-Bold.ttf", 15.0f);
    g_ui.font_brand = font_load("assets/fonts/IBMPlexSans-BoldItalic.ttf", 26.0f);
    g_ui.font_mono  = font_load("assets/fonts/IBMPlexMono-Regular.ttf", 14.0f);
    if (!g_ui.font_body || !g_ui.font_bold || !g_ui.font_brand || !g_ui.font_mono) {
        printf("[ui] a required font failed to load\n");
        return 0;
    }

    g_ui.icon_scene      = svg_icon_load("assets/icons/scene.svg", 32);
    g_ui.icon_outliner   = svg_icon_load("assets/icons/outliner.svg", 32);
    g_ui.icon_properties = svg_icon_load("assets/icons/properties.svg", 32);
    g_ui.icon_console    = svg_icon_load("assets/icons/console.svg", 32);
    g_ui.icon_chat        = svg_icon_load("assets/icons/repl.svg", 32);  /* Zenith's Chat icon, reused verbatim per instruction */
    g_ui.icon_node_editor = svg_icon_load("assets/icons/node_editor.svg", 32);
    g_ui.icon_curve_editor= svg_icon_load("assets/icons/curve_editor.svg", 32);
    g_ui.icon_undo         = svg_icon_load("assets/icons/undo.svg", 24);
    g_ui.icon_redo         = svg_icon_load("assets/icons/redo.svg", 24);

    /* Default layout: golden-ratio split, Scene | (Outliner / Properties).
     * See ui.h's Area comment and phi.md's Native UI System section for
     * why this is the target model (Blender's recursive area-split, not
     * Zenith's fixed-slot dock). */
    Area *scene_leaf = (Area *)calloc(1, sizeof(Area));
    scene_leaf->kind = AREA_LEAF; scene_leaf->panel_type = PANEL_SCENE;

    Area *outliner_leaf = (Area *)calloc(1, sizeof(Area));
    outliner_leaf->kind = AREA_LEAF; outliner_leaf->panel_type = PANEL_OUTLINER;

    Area *properties_leaf = (Area *)calloc(1, sizeof(Area));
    properties_leaf->kind = AREA_LEAF; properties_leaf->panel_type = PANEL_PROPERTIES;

    Area *right_col = (Area *)calloc(1, sizeof(Area));
    right_col->kind = AREA_SPLIT_V;
    right_col->split = UI_INV_PHI;  /* Outliner (top) gets the larger golden fraction */
    right_col->child[0] = outliner_leaf;
    right_col->child[1] = properties_leaf;

    Area *root = (Area *)calloc(1, sizeof(Area));
    root->kind = AREA_SPLIT_H;
    root->split = UI_INV_PHI;  /* Scene (left) gets the larger golden fraction */
    root->child[0] = scene_leaf;
    root->child[1] = right_col;

    g_ui.root = root;
    g_ui.panels[0] = scene_leaf; g_ui.panels[1] = outliner_leaf; g_ui.panels[2] = properties_leaf;
    g_ui.panel_count = 3;

    printf("[ui] init ok: fonts body=%p bold=%p brand=%p mono=%p, icons scene=%u outliner=%u properties=%u console=%u chat=%u\n",
           (void *)g_ui.font_body, (void *)g_ui.font_bold, (void *)g_ui.font_brand, (void *)g_ui.font_mono,
           g_ui.icon_scene.texture, g_ui.icon_outliner.texture, g_ui.icon_properties.texture,
           g_ui.icon_console.texture, g_ui.icon_chat.texture);
    gl_check("ui_init");
    return 1;
}

void ui_destroy(void) {
    font_destroy(g_ui.font_body); font_destroy(g_ui.font_bold);
    font_destroy(g_ui.font_brand); font_destroy(g_ui.font_mono);
    svg_icon_destroy(&g_ui.icon_scene); svg_icon_destroy(&g_ui.icon_outliner);
    svg_icon_destroy(&g_ui.icon_properties); svg_icon_destroy(&g_ui.icon_console);
    svg_icon_destroy(&g_ui.icon_chat); svg_icon_destroy(&g_ui.icon_node_editor);
    svg_icon_destroy(&g_ui.icon_curve_editor); svg_icon_destroy(&g_ui.icon_undo); svg_icon_destroy(&g_ui.icon_redo);
}

/* ---- Layout ---- */
static void layout_area(Area *a, float x, float y, float w, float h) {
    a->x = x; a->y = y; a->w = w; a->h = h;
    if (a->kind == AREA_SPLIT_H) {
        float w0 = w * a->split;
        layout_area(a->child[0], x, y, w0, h);
        layout_area(a->child[1], x + w0, y, w - w0, h);
    } else if (a->kind == AREA_SPLIT_V) {
        float h0 = h * a->split;
        layout_area(a->child[0], x, y, w, h0);
        layout_area(a->child[1], x, y + h0, w, h - h0);
    }
}

void ui_layout(int window_w, int window_h) {
    g_ui.screen_w = window_w;
    g_ui.screen_h = window_h;
    if (!g_ui.root) return;
    layout_area(g_ui.root, 0.0f, UI_TOP_CHROME_H, (float)window_w, (float)window_h - UI_TOP_CHROME_H);
}

static SvgIcon *icon_for_panel(PanelType t) {
    switch (t) {
        case PANEL_SCENE: return &g_ui.icon_scene;
        case PANEL_OUTLINER: return &g_ui.icon_outliner;
        case PANEL_PROPERTIES: return &g_ui.icon_properties;
        case PANEL_CONSOLE: return &g_ui.icon_console;
        case PANEL_CHAT: return &g_ui.icon_chat;
        case PANEL_NODE_EDITOR: return &g_ui.icon_node_editor;
        case PANEL_CURVE_EDITOR: return &g_ui.icon_curve_editor;
        default: return NULL;
    }
}

/* ---- Palette ----
 * The branding bar (only) copies https://float64co.github.io's actual
 * color palette, read from its real page source (`--bg`/`--acc`/etc. CSS
 * custom properties, and the brand mark's own `#87CEEB` pill color) —
 * this is explicitly a Float64 project, so this is Phi's own brand
 * identity, not someone else's being borrowed. Every OTHER color in this
 * file (menu row, panel chrome, panel content, dropdowns, context menu)
 * uses a separate reference project's actual dark-theme palette instead
 * (`col::PANEL_BG`/`WIDGET`/`TEXT`/etc. from that project's DrawBatch.h,
 * read this session) — a deliberate two-tier scheme: light branded top
 * bar over a dark professional editor body, the same split VSCode/Blender
 * and most serious creative tools use, not a mismatch. */
#define UI_BRAND_BG_R 0.961f
#define UI_BRAND_BG_G 0.969f
#define UI_BRAND_BG_B 0.980f
#define UI_BRAND_BORDER_R 0.878f
#define UI_BRAND_BORDER_G 0.894f
#define UI_BRAND_BORDER_B 0.918f
#define UI_BRAND_ACCENT_R 0.0f      /* float64's --acc: #00bfff */
#define UI_BRAND_ACCENT_G 0.749f
#define UI_BRAND_ACCENT_B 1.0f
#define UI_BRAND_PILL_R 0.529f      /* float64's own brand-mark pill: #87CEEB */
#define UI_BRAND_PILL_G 0.808f
#define UI_BRAND_PILL_B 0.922f
#define UI_BRAND_TEXT_R 0.2f        /* float64's --body: #333333 */
#define UI_BRAND_TEXT_G 0.2f
#define UI_BRAND_TEXT_B 0.2f

/* Dark editor-body palette, values copied from that reference project's
 * col:: namespace (DrawBatch.h) — same 0-255 numbers, divided by 255. */
#define UI_ZEN_PANEL_BG_R 0.094f
#define UI_ZEN_PANEL_BG_G 0.094f
#define UI_ZEN_PANEL_BG_B 0.094f
#define UI_ZEN_PANEL_BG_A 0.902f
#define UI_ZEN_PANEL_HD_R 0.071f
#define UI_ZEN_PANEL_HD_G 0.071f
#define UI_ZEN_PANEL_HD_B 0.071f
#define UI_ZEN_WIDGET_R 0.141f
#define UI_ZEN_WIDGET_G 0.141f
#define UI_ZEN_WIDGET_B 0.141f
#define UI_ZEN_WIDGET_H_R 0.188f    /* hovered */
#define UI_ZEN_WIDGET_H_G 0.188f
#define UI_ZEN_WIDGET_H_B 0.188f
#define UI_ZEN_ACCENT_R 0.0f        /* active/accent */
#define UI_ZEN_ACCENT_G 0.478f
#define UI_ZEN_ACCENT_B 0.800f
#define UI_ZEN_TEXT_R 0.980f
#define UI_ZEN_TEXT_G 0.980f
#define UI_ZEN_TEXT_B 0.996f
#define UI_ZEN_TEXT_DIM_R 0.725f
#define UI_ZEN_TEXT_DIM_G 0.725f
#define UI_ZEN_TEXT_DIM_B 0.765f
#define UI_ZEN_BORDER_R 0.157f
#define UI_ZEN_BORDER_G 0.157f
#define UI_ZEN_BORDER_B 0.157f

static void draw_branding_bar(void) {
    ui_rect(0, 0, (float)g_ui.screen_w, UI_BAR_H, UI_BRAND_BG_R, UI_BRAND_BG_G, UI_BRAND_BG_B, 1.0f);
    ui_rect(0, UI_BAR_H - 1.0f, (float)g_ui.screen_w, 1.0f, UI_BRAND_BORDER_R, UI_BRAND_BORDER_G, UI_BRAND_BORDER_B, 1.0f);

    /* Two conjoined brand-mark pills, matching float64co.github.io's own
     * markup structure exactly: <span id=float64> (bold italic, white on
     * #87CEEB) directly adjacent to <span id=welcome> (bold, white on
     * black, not italic) -- no gap between them, one flush unit. Phi's
     * version reads "Float64" + "Phi" instead of the site's own second
     * word, since this bar is announcing Phi specifically. Per explicit
     * request, the pills sit flush against the viewport's own left edge
     * (x=0, not inset) and flush against the bar's own top and bottom
     * (pill height == UI_BAR_H exactly, no vertical margin) -- unlike the
     * site's version, which has nav padding around it and its own 5px
     * pill padding inside a taller bar. */
    float brand_font = UI_BAR_H * 0.52f;
    float pad_x = brand_font * 0.42f;
    const char *t1 = "Float64", *t2 = "Phi";
    float w1 = font_text_width(g_ui.font_brand, t1, brand_font) + pad_x * 2.0f;
    float w2 = font_text_width(g_ui.font_bold,  t2, brand_font) + pad_x * 2.0f;
    float text_y = (UI_BAR_H - brand_font) * 0.5f - brand_font * 0.12f + 2.0f;

    ui_rect(0.0f, 0.0f, w1, UI_BAR_H, UI_BRAND_PILL_R, UI_BRAND_PILL_G, UI_BRAND_PILL_B, 1.0f);
    ui_text_draw(pad_x, text_y, t1, g_ui.font_brand, brand_font, 1.0f, 1.0f, 1.0f, 1.0f);

    ui_rect(w1, 0.0f, w2, UI_BAR_H, 0.0f, 0.0f, 0.0f, 1.0f);
    ui_text_draw(w1 + pad_x, text_y, t2, g_ui.font_bold, brand_font, 1.0f, 1.0f, 1.0f, 1.0f);
}

/* Main menu row, directly under the branding bar — Zenith's dark palette,
 * not float64's (see the palette comment above). Plain labels for now
 * (File/Edit/View/Help, the standard desktop-app set) with no dropdown
 * content yet — this proves the chrome has a place for a real menu system
 * to land in, not a finished menu bar. */
static void draw_menu_row(void) {
    float y = UI_BAR_H;
    ui_rect(0, y, (float)g_ui.screen_w, UI_MENU_H, UI_ZEN_PANEL_HD_R, UI_ZEN_PANEL_HD_G, UI_ZEN_PANEL_HD_B, 1.0f);
    ui_rect(0, y + UI_MENU_H - 1.0f, (float)g_ui.screen_w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);

    static const char *items[] = { "File", "Edit", "View", "Help" };
    float x = 6.0f;
    float text_y = y + (UI_MENU_H - UI_FONT_SIZE) * 0.35f + 2.0f;
    for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); i++) {
        float w = font_text_width(g_ui.font_body, items[i], UI_FONT_SIZE) + 20.0f;
        ui_text_draw(x + 10.0f, text_y, items[i], g_ui.font_body, UI_FONT_SIZE,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        x += w;
    }

    /* Undo/redo, moved here from the branding bar and scaled down to fit
     * this shorter row (UI_MENU_H, not UI_BAR_H), to the right of the
     * File/Edit/View/Help entries above -- Zenith's icons, reused verbatim
     * per the original instruction. No real undo stack exists yet to drive
     * these; functional wiring is follow-up work once there's something to
     * undo. */
    float btn = UI_MENU_H * 0.55f, gap = 6.0f;
    float icon_y = y + (UI_MENU_H - btn) * 0.5f;
    x += 14.0f;  /* extra breathing room after "Help" before these start */
    ui_icon_draw(x, icon_y, btn, g_ui.icon_undo, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 0.9f);
    x += btn + gap;
    ui_icon_draw(x, icon_y, btn, g_ui.icon_redo, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 0.9f);
}

/* ---- Per-area chrome: background, border, and the Blender-authentic
 * per-panel type-switcher icon button in the area's own top-right corner
 * (not a single global strip — see ui.h/phi.md's design note). ---- */
static int point_in_rect(float px, float py, float x, float y, float w, float h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void draw_area_chrome(Area *a) {
    ui_rect(a->x, a->y, a->w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
    ui_rect(a->x, a->y, 1.0f, a->h, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);

    float bx = a->x + a->w - UI_TYPE_ICON_SIZE - 4.0f;
    float by = a->y + 4.0f;
    SvgIcon *icon = icon_for_panel(a->panel_type);
    if (icon) ui_icon_draw(bx, by, UI_TYPE_ICON_SIZE, *icon, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 0.85f);

    if (a->type_menu_open) {
        float menu_w = 170.0f, row_h = 24.0f;
        float menu_h = row_h * PANEL_TYPE_COUNT;
        float menu_x = bx + UI_TYPE_ICON_SIZE - menu_w;
        float menu_y = by + UI_TYPE_ICON_SIZE + 2.0f;
        ui_rect(menu_x, menu_y, menu_w, menu_h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 0.98f);
        ui_rect(menu_x, menu_y, menu_w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
        for (int i = 0; i < PANEL_TYPE_COUNT; i++) {
            float ry = menu_y + i * row_h;
            SvgIcon *ri = icon_for_panel((PanelType)i);
            if (ri) ui_icon_draw(menu_x + 6.0f, ry + 3.0f, 18.0f, *ri, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 0.85f);
            ui_text_draw(menu_x + 30.0f, ry + 3.0f, PANEL_NAMES[i], g_ui.font_body, 13.0f,
                         UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        }
    }
}

/* ---- Panel content ---- */
static void draw_panel_scene(Area *a, const UIRenderContext *ctx) {
    if (!ctx->gbuf || !ctx->renderer) return;
    int px = (int)a->x, py_top = (int)a->y, pw = (int)a->w, ph = (int)a->h;
    if (pw < 4 || ph < 4) return;

    /* GL viewport offset is bottom-left origin; Area's (x,y) is top-left,
     * y-down -- convert once here. */
    int gl_x = px;
    int gl_y = g_ui.screen_h - py_top - ph;

    renderer_resize(ctx->renderer, pw, ph);
    gbuffer_resize(ctx->gbuf, pw, ph);
    gbuffer_set_viewport_offset(ctx->gbuf, gl_x, gl_y);

    gbuffer_begin_geometry_pass(ctx->gbuf, ctx->sky_color);
    if (ctx->draw_scene_content) ctx->draw_scene_content(ctx->draw_scene_userdata);
    gbuffer_render_shadow_map(ctx->gbuf, ctx->world_mesh, ctx->light_dir);
    float inv_vp[16];
    renderer_get_inverse_view_proj(ctx->renderer, inv_vp);
    gbuffer_resolve(ctx->gbuf, ctx->light_dir, ctx->sky_color, inv_vp);
    renderer_end_frame(ctx->renderer);

    /* gbuffer_resolve's FXAA pass just changed which program/VAO/blend
     * state is bound and cleared the depth test back on -- the 2D UI
     * pipeline needs its own state, not whatever the 3D pass left behind. */
    glDisable(GL_DEPTH_TEST);
}

/* Zebra-striped, full-panel-width row backgrounds — Blender's own list-row
 * convention (rather than left-aligned bare text trailing off into blank
 * space on the right, which read as "empty space to the right of the
 * Outliner" against this panel's ~38%-of-window width). Row index is
 * shared across every row-emitting section below via *row_index so the
 * stripe alternates continuously (world mesh, then object, then players),
 * not restarting per section. */
static void outliner_row_bg(Area *a, float y, float row_h, int row_index) {
    if (row_index % 2 == 1) {
        ui_rect(a->x + 1.0f, y - 2.0f, a->w - 2.0f, row_h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 0.5f);
    }
}

static void draw_panel_outliner(Area *a, const UIRenderContext *ctx) {
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    float x = a->x + UI_PANEL_PAD, y = a->y + UI_PANEL_PAD;
    float row_h = 20.0f;
    int row_index = 0;
    ui_text_draw(x, y, "Outliner", g_ui.font_bold, 15.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y += row_h + 6.0f;

    char line[128];
    if (ctx->world_mesh) {
        outliner_row_bg(a, y, row_h, row_index++);
        snprintf(line, sizeof(line), "World Mesh (%d verts)", ctx->world_mesh->count);
        ui_text_draw(x, y, line, g_ui.font_body, 14.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        y += row_h;
    }
    if (ctx->test_obj_loaded && ctx->test_obj) {
        int sel = (g_ui.selected_object_id == 4000u + (unsigned int)ctx->test_obj->id);
        if (sel) {
            ui_rect(a->x + 1.0f, y - 2.0f, a->w - 2.0f, row_h, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 0.35f);
        } else {
            outliner_row_bg(a, y, row_h, row_index);
        }
        row_index++;
        snprintf(line, sizeof(line), "MeshObject #%d", ctx->test_obj->id);
        ui_text_draw(x, y, line, g_ui.font_body, 14.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        y += row_h;
    }
    if (ctx->gs) {
        for (int i = 0; i < ctx->gs->num_players; i++) {
            const Player *p = &ctx->gs->players[i];
            if (!p->alive) continue;
            outliner_row_bg(a, y, row_h, row_index++);
            snprintf(line, sizeof(line), "%s #%d%s", p->is_bot ? "Bot" : "Player", p->id,
                     p->id == (uint8_t)ctx->local_player_id ? " (you)" : "");
            ui_text_draw(x, y, line, g_ui.font_body, 14.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
            y += row_h;
            if (y > a->y + a->h - row_h) break;  /* no scrolling yet -- first pass, see phi.md */
        }
    }
}

static void draw_panel_properties(Area *a, const UIRenderContext *ctx) {
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    float x = a->x + UI_PANEL_PAD, y = a->y + UI_PANEL_PAD;
    ui_text_draw(x, y, "Properties", g_ui.font_bold, 15.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y += 26.0f;

    if (ctx->test_obj_loaded && ctx->test_obj &&
        g_ui.selected_object_id == 4000u + (unsigned int)ctx->test_obj->id) {
        char line[96];
        snprintf(line, sizeof(line), "MeshObject #%d", ctx->test_obj->id);
        ui_text_draw(x, y, line, g_ui.font_body, 14.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        y += 22.0f;
        snprintf(line, sizeof(line), "Position: %.2f, %.2f, %.2f",
                 ctx->test_obj->position.x, ctx->test_obj->position.y, ctx->test_obj->position.z);
        ui_text_draw(x, y, line, g_ui.font_mono, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 20.0f;
        snprintf(line, sizeof(line), "Orientation: %.2f, %.2f, %.2f, %.2f",
                 ctx->test_obj->orientation.x, ctx->test_obj->orientation.y,
                 ctx->test_obj->orientation.z, ctx->test_obj->orientation.w);
        ui_text_draw(x, y, line, g_ui.font_mono, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 20.0f;
        ui_text_draw(x, y, ctx->test_obj->is_static ? "Static: yes" : "Static: no", g_ui.font_mono, 13.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    } else {
        ui_text_draw(x, y, "Nothing selected", g_ui.font_body, 14.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 22.0f;
        ui_text_draw(x, y, "Click an object in the Scene or Outliner.", g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    }
}

static void draw_panel_console(Area *a, const UIRenderContext *ctx) {
    float x = a->x + UI_PANEL_PAD, y = a->y + a->h - UI_PANEL_PAD - 18.0f;
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_HD_R, UI_ZEN_PANEL_HD_G, UI_ZEN_PANEL_HD_B, 0.95f);
    if (!ctx->console) return;

    /* Input row pinned to the bottom, log scrolling up from just above it
     * -- newest line closest to the input, matching normal terminal/chat
     * scrollback orientation. */
    char prompt[CONSOLE_INPUT_LEN + 4];
    snprintf(prompt, sizeof(prompt), "> %s", ctx->console->input);
    ui_text_draw(x, y, prompt, g_ui.font_mono, 13.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y -= 20.0f;

    for (int i = ctx->console->log_count - 1; i >= 0 && y > a->y + UI_PANEL_PAD; i--) {
        ui_text_draw(x, y, ctx->console->log[i], g_ui.font_mono, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y -= 18.0f;
    }
}

static void draw_panel_chat(Area *a, const UIRenderContext *ctx) {
    (void)ctx;
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    float x = a->x + UI_PANEL_PAD, y = a->y + UI_PANEL_PAD;
    ui_text_draw(x, y, "Chat", g_ui.font_bold, 15.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y += 26.0f;
    /* Explicitly not connected to a real LLM yet -- that needs a
     * server-side proxy per phi.md's Hard Architectural Decision that the
     * API key never ships to the client. This is the panel shell only. */
    ui_text_draw(x, y, "Not connected to an LLM yet.", g_ui.font_body, 14.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    y += 20.0f;
    ui_text_draw(x, y, "Needs a server-side API proxy (see phi.md).", g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);

    float input_h = 28.0f;
    float input_y = a->y + a->h - UI_PANEL_PAD - input_h;
    ui_rect(x, input_y, a->w - UI_PANEL_PAD * 2.0f, input_h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 1.0f);
    ui_rect(x, input_y, a->w - UI_PANEL_PAD * 2.0f, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
    ui_text_draw(x + 8.0f, input_y + 6.0f, "Type a message...", g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
}

static void draw_panel_stub(Area *a, const char *name) {
    ui_text_draw(a->x + UI_PANEL_PAD, a->y + UI_PANEL_PAD, name, g_ui.font_bold, 15.0f,
                 UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    ui_text_draw(a->x + UI_PANEL_PAD, a->y + UI_PANEL_PAD + 24.0f, "Not implemented yet.",
                 g_ui.font_body, 14.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
}

static void draw_leaf(Area *a, const UIRenderContext *ctx) {
    switch (a->panel_type) {
        case PANEL_SCENE:      draw_panel_scene(a, ctx); break;
        case PANEL_OUTLINER:   draw_panel_outliner(a, ctx); break;    /* fills its own background, see above */
        case PANEL_PROPERTIES: draw_panel_properties(a, ctx); break;  /* ditto */
        case PANEL_CONSOLE:    draw_panel_console(a, ctx); break;
        case PANEL_CHAT:       draw_panel_chat(a, ctx); break;        /* ditto */
        case PANEL_NODE_EDITOR:  ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A); draw_panel_stub(a, "Node Editor"); break;
        case PANEL_CURVE_EDITOR: ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A); draw_panel_stub(a, "Curve Editor"); break;
        default: break;
    }
    /* draw_panel_scene's G-buffer FXAA blit leaves the GL viewport set to
     * the Scene panel's own sub-rectangle (gbuffer_set_viewport_offset),
     * not the full window -- every ui_rect/ui_text_draw/ui_icon_draw call
     * (draw_area_chrome below, and every OTHER panel drawn after this one
     * in the same walk_and_draw pass) computes its NDC position assuming a
     * full-window viewport via u_screen_size, so without this reset those
     * draws get remapped into whatever smaller viewport the Scene panel
     * happened to leave behind -- e.g. the Outliner/Properties panels
     * rendering squashed into the Scene panel's own on-screen rect instead
     * of their own, to the right of it. Resetting unconditionally here
     * (not just once at the end of ui_render) means every panel's chrome
     * and every subsequent panel starts from a known-correct viewport,
     * regardless of draw order or which panel type precedes it. */
    glViewport(0, 0, g_ui.screen_w, g_ui.screen_h);
    draw_area_chrome(a);
}

static void walk_and_draw(Area *a, const UIRenderContext *ctx) {
    if (a->kind == AREA_LEAF) { draw_leaf(a, ctx); return; }
    walk_and_draw(a->child[0], ctx);
    walk_and_draw(a->child[1], ctx);
}

void ui_render(const UIRenderContext *ctx) {
    /* Panel content (including the Scene panel's own full G-buffer
     * pipeline run) FIRST, branding bar LAST, so the bar always paints
     * over the top rather than a panel that happens to extend under it
     * (none do in the default layout — root's own y-origin already starts
     * below UI_BAR_H, see ui_layout — but this ordering is the correct
     * one regardless of what future layouts do). */
    if (g_ui.root) walk_and_draw(g_ui.root, ctx);

    glViewport(0, 0, g_ui.screen_w, g_ui.screen_h);
    glDisable(GL_DEPTH_TEST);
    draw_branding_bar();
    draw_menu_row();

    if (g_ui.ctx_menu_open) {
        float menu_w = 180.0f, row_h = 24.0f;
        static const char *items[] = { "Add > Mesh Object", "Delete", "Frame Selected", "Frame All", "Deselect All" };
        int n = (int)(sizeof(items) / sizeof(items[0]));
        float menu_h = row_h * n;
        ui_rect(g_ui.ctx_menu_x, g_ui.ctx_menu_y, menu_w, menu_h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 0.98f);
        ui_rect(g_ui.ctx_menu_x, g_ui.ctx_menu_y, menu_w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
        for (int i = 0; i < n; i++) {
            ui_text_draw(g_ui.ctx_menu_x + 10.0f, g_ui.ctx_menu_y + i * row_h + 4.0f, items[i],
                         g_ui.font_body, 13.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        }
    }
    gl_check("ui_render");
}

/* ---- Input ----
 * First-pass hit-testing: walk every leaf, check its type-switcher icon
 * and (if open) dropdown rows, then the global context menu. Camera-look/
 * fire input etc. stay main.c's job for clicks that land in a Scene
 * panel's content and nothing UI-owned claims first. */
static int hit_test_area(Area *a, int x, int y, int button, int pressed, const UIRenderContext *ctx) {
    if (a->kind != AREA_LEAF) {
        return hit_test_area(a->child[0], x, y, button, pressed, ctx) ||
               hit_test_area(a->child[1], x, y, button, pressed, ctx);
    }
    float bx = a->x + a->w - UI_TYPE_ICON_SIZE - 4.0f;
    float by = a->y + 4.0f;
    if (button == 0 && pressed && point_in_rect((float)x, (float)y, bx, by, UI_TYPE_ICON_SIZE, UI_TYPE_ICON_SIZE)) {
        a->type_menu_open = !a->type_menu_open;
        return 1;
    }
    if (a->type_menu_open) {
        float menu_w = 170.0f, row_h = 24.0f;
        float menu_h = row_h * PANEL_TYPE_COUNT;
        float menu_x = bx + UI_TYPE_ICON_SIZE - menu_w;
        float menu_y = by + UI_TYPE_ICON_SIZE + 2.0f;
        if (button == 0 && pressed) {
            if (point_in_rect((float)x, (float)y, menu_x, menu_y, menu_w, menu_h)) {
                int row = (int)(((float)y - menu_y) / row_h);
                if (row >= 0 && row < PANEL_TYPE_COUNT) a->panel_type = (PanelType)row;
                a->type_menu_open = 0;
                return 1;
            } else {
                a->type_menu_open = 0;  /* click-away dismisses, same convention as the context menu */
            }
        }
    }
    /* Click inside a leaf's content (not its chrome) that isn't otherwise
     * claimed: for Outliner specifically, treat it as a selection click —
     * this is the ONE piece of per-panel-type interaction wired this
     * pass, everything else (Console/Chat text entry, Properties editing)
     * is display-only for now, matching this loop's scoped-down first
     * pass for those panels. */
    if (a->panel_type == PANEL_OUTLINER && button == 0 && pressed &&
        point_in_rect((float)x, (float)y, a->x, a->y, a->w, a->h) &&
        ctx->test_obj_loaded && ctx->test_obj) {
        /* Row math must match draw_panel_outliner's layout exactly (world
         * mesh row, then the test object's row) -- selects the test
         * object if the click lands on/after its row. First-pass
         * approximation (fixed row assumption), not a real hit-list —
         * fine for two rows, would need real per-row hit rects once
         * Outliner's content is dynamic. */
        float row0_y = a->y + UI_PANEL_PAD + 20.0f + 6.0f;
        if ((float)y >= row0_y + 20.0f && (float)y < row0_y + 40.0f) {
            g_ui.selected_object_id = 4000u + (unsigned int)ctx->test_obj->id;
        }
        return 1;
    }
    return 0;
}

int ui_on_mouse_button(int x, int y, int button, int pressed, const UIRenderContext *ctx) {
    if (g_ui.ctx_menu_open) {
        float menu_w = 180.0f, row_h = 24.0f;
        static const char *items[] = { "Add > Mesh Object", "Delete", "Frame Selected", "Frame All", "Deselect All" };
        int n = (int)(sizeof(items) / sizeof(items[0]));
        float menu_h = row_h * n;
        if (button == 0 && pressed) {
            if (point_in_rect((float)x, (float)y, g_ui.ctx_menu_x, g_ui.ctx_menu_y, menu_w, menu_h)) {
                int row = (int)(((float)y - g_ui.ctx_menu_y) / row_h);
                if (row >= 0 && row < n) printf("[ui] context menu: '%s' (mechanism proven, action not yet wired)\n", items[row]);
            }
            g_ui.ctx_menu_open = 0;
            return 1;
        }
        if (button == 1 && pressed) { g_ui.ctx_menu_open = 0; return 1; }
    }
    if (y < (int)UI_TOP_CHROME_H) return 1;  /* branding bar + menu row claim the whole strip, nothing to route through it yet */
    if (g_ui.root && hit_test_area(g_ui.root, x, y, button, pressed, ctx)) return 1;
    return 0;
}

void ui_on_mouse_move(int x, int y) { (void)x; (void)y; /* hover states are a follow-up, not needed for this pass */ }

void ui_open_scene_context_menu(int x, int y) {
    g_ui.ctx_menu_open = 1;
    g_ui.ctx_menu_x = (float)x;
    g_ui.ctx_menu_y = (float)y;
}

int ui_is_context_menu_open(void) { return g_ui.ctx_menu_open; }

void ui_set_selected_object(unsigned int object_id) { g_ui.selected_object_id = object_id; }
unsigned int ui_get_selected_object(void) { return g_ui.selected_object_id; }

/* Returns the Scene panel's current screen rect (needed by main.c to know
 * whether a click/raycast should be treated as "inside the 3D viewport"
 * at all, and where within it, since the Scene panel no longer always
 * fills the whole window). Searches the tree fresh each call rather than
 * caching a pointer, since panel types can change at runtime via the
 * per-area switcher. Returns 0 (all fields untouched) if no Scene panel
 * exists in the current layout. */
static int find_scene_rect_r(Area *a, float *x, float *y, float *w, float *h) {
    if (a->kind == AREA_LEAF) {
        if (a->panel_type != PANEL_SCENE) return 0;
        *x = a->x; *y = a->y; *w = a->w; *h = a->h;
        return 1;
    }
    return find_scene_rect_r(a->child[0], x, y, w, h) || find_scene_rect_r(a->child[1], x, y, w, h);
}
int ui_get_scene_rect(float *x, float *y, float *w, float *h) {
    if (!g_ui.root) return 0;
    return find_scene_rect_r(g_ui.root, x, y, w, h);
}
