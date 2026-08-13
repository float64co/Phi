#include "ui.h"
#include "area_tree.h"
#include "phi_prop_registry.h"
#include "mp_port.h"
#include "meshobject.h"
#include "renderer.h"
#include "gbuffer.h"
#include "octree_render.h"
#include "net.h"
#include "console.h"
#include "phi_platform.h"   /* phi_platform_now, for the console caret blink */
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
    SvgIcon icon_node_editor, icon_curve_editor, icon_undo, icon_redo, icon_asset_browser, icon_python_panel;

    Area *root;
    Area *panels[16];   /* flat list of every leaf, for hit-testing/iteration */
    int   panel_count;

    unsigned int selected_object_id;  /* 0xFFFFFFFF = none, matches gbuffer_pick_object_id's sentinel */

    /* 3D scene right-click context menu -- see ui_open_scene_context_menu() */
    int   ctx_menu_open;
    float ctx_menu_x, ctx_menu_y;
    CtxMenuAction ctx_menu_pending_action;  /* see ui_poll_context_menu_action() */

    /* Blender-style area border drag-to-resize (see area_tree_find_border_hit/
     * ui_update_area_drag) -- NULL when not resizing. Set on a press
     * inside a border's hit strip (ui_on_mouse_button), updated every
     * frame via ui_update_area_drag (called from main.c's main_loop, the
     * same "continuous per-frame update while a button stays held" shape
     * the gizmo drag already established), cleared on release. */
    Area *resizing_area;
    /* Purely visual -- which border (if any) to highlight because the
     * mouse is over it right now, not being dragged. Updated by
     * ui_on_mouse_move, which main.c now actually calls every frame
     * (it used to be declared but never called at all). */
    Area *hover_border;

    /* Blender-style Area menu (Split Horizontal/Vertical, Join with
     * Sibling) -- opened by right-clicking a panel's own type-switcher
     * icon, the same spot Blender's own per-area options menu lives
     * behind. A menu-driven equivalent of Blender's corner-drag gesture,
     * not a pixel-for-pixel replication of it -- see phi.md's note on
     * this tradeoff. */
    int   area_menu_open;
    Area *area_menu_target;
    float area_menu_x, area_menu_y;

    /* Python Panel scroll (see draw_panel_python) -- lives here rather
     * than in mp_port.c's state, since it's a pure rendering/hover
     * concern with no Python-visible meaning, same reasoning ChatState/
     * PyConsoleState's own scroll_offset fields already establish for
     * their panels. Top-anchored (0 = scrolled to the first returned
     * line), same convention as the Asset Browser's list_scroll_offset. */
    int   python_panel_scroll;
} UIState;

static UIState g_ui;

static const char *PANEL_NAMES[PANEL_TYPE_COUNT] = {
    "Scene", "Outliner", "Properties", "Python Console", "Chat", "Node Editor (not implemented yet)", "Curve Editor (not implemented yet)", "Asset Browser", "Python Panel"
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
    g_ui.icon_asset_browser = svg_icon_load("assets/icons/asset_browser.svg", 32);
    g_ui.icon_python_panel  = svg_icon_load("assets/icons/python_panel.svg", 32);

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
    /* Properties (bottom) now gets the larger golden fraction, Outliner
     * (top) the smaller -- swapped from the original Outliner-larger/
     * Properties-smaller split per an explicit request, top/bottom order
     * unchanged. child[0]'s height is h*split (see layout_area), so
     * giving child[1]/Properties the larger share means split itself is
     * the SMALLER fraction (1 - UI_INV_PHI), not UI_INV_PHI. */
    right_col->split = 1.0f - UI_INV_PHI;
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

    printf("[ui] init ok: fonts body=%p bold=%p brand=%p mono=%p, icons scene=%u outliner=%u properties=%u console=%u chat=%u asset_browser=%u\n",
           (void *)g_ui.font_body, (void *)g_ui.font_bold, (void *)g_ui.font_brand, (void *)g_ui.font_mono,
           g_ui.icon_scene.texture, g_ui.icon_outliner.texture, g_ui.icon_properties.texture,
           g_ui.icon_console.texture, g_ui.icon_chat.texture, g_ui.icon_asset_browser.texture);
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
    svg_icon_destroy(&g_ui.icon_asset_browser);
    svg_icon_destroy(&g_ui.icon_python_panel);
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

/* ---- Area tree structural edits: split (create) / join (delete) ----
 * The actual tree mutation (area_tree_split/join_with_sibling/etc.) lives
 * in area_tree.c/.h, a GL-free module extracted so it can be unit-tested
 * standalone -- ui.c just owns the interactive/visual side (drag state,
 * hover highlighting, the Area menu) and calls into it. */

/* Called once per frame from main.c's main_loop (mirrors the gizmo drag
 * block's own shape exactly): while g_ui.resizing_area is set and the
 * button's still down, keep the border glued to the mouse; on release,
 * end the drag. A no-op, including not touching anything, when nothing's
 * being resized. */
void ui_update_area_drag(int mouse_x, int mouse_y, int lmb_down) {
    if (!g_ui.resizing_area) return;
    if (lmb_down) {
        area_tree_resize_to_mouse(g_ui.resizing_area, mouse_x, mouse_y);
    } else {
        g_ui.resizing_area = NULL;
    }
}

int ui_is_resizing_area(void) { return g_ui.resizing_area != NULL; }

static SvgIcon *icon_for_panel(PanelType t) {
    switch (t) {
        case PANEL_SCENE: return &g_ui.icon_scene;
        case PANEL_OUTLINER: return &g_ui.icon_outliner;
        case PANEL_PROPERTIES: return &g_ui.icon_properties;
        case PANEL_CONSOLE: return &g_ui.icon_console;
        case PANEL_CHAT: return &g_ui.icon_chat;
        case PANEL_NODE_EDITOR: return &g_ui.icon_node_editor;
        case PANEL_CURVE_EDITOR: return &g_ui.icon_curve_editor;
        case PANEL_ASSET_BROWSER: return &g_ui.icon_asset_browser;
        case PANEL_PYTHON: return &g_ui.icon_python_panel;
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

/* Top-left corner of a panel's own type-switcher icon -- shared by the
 * drawing code below and hit_test_area's click routing so the two can
 * never disagree about where the icon actually is. */
static void type_icon_rect(Area *a, float *bx, float *by) {
    *bx = a->x + 4.0f;
    *by = a->y + 4.0f;
}

/* Widest row text in the type-switcher dropdown ("Node Editor (not
 * implemented yet)"/"Curve Editor (not implemented yet)" are the long
 * ones) drives the menu's width -- shared by drawing and hit-testing
 * (mirroring type_icon_rect above) so a click can't land past where the
 * menu is actually drawn or vice versa. */
static float type_menu_width(void) {
    float max_w = 0.0f;
    for (int i = 0; i < PANEL_TYPE_COUNT; i++) {
        float w = font_text_width(g_ui.font_body, PANEL_NAMES[i], 13.0f);
        if (w > max_w) max_w = w;
    }
    return max_w + 30.0f + 14.0f;  /* +30 icon-then-text offset, +14 right margin */
}

static void draw_area_chrome(Area *a) {
    ui_rect(a->x, a->y, a->w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
    ui_rect(a->x, a->y, 1.0f, a->h, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);

    float bx, by;
    type_icon_rect(a, &bx, &by);
    SvgIcon *icon = icon_for_panel(a->panel_type);
    if (icon) ui_icon_draw(bx, by, UI_TYPE_ICON_SIZE, *icon, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 0.85f);

    if (a->type_menu_open) {
        float menu_w = type_menu_width(), row_h = 24.0f;
        float menu_h = row_h * PANEL_TYPE_COUNT;
        /* Spawns directly underneath the icon now that the icon lives in
         * the top-left corner -- left edges aligned, growing down and to
         * the right, rather than the old top-right icon's right-aligned
         * leftward-growing menu. */
        float menu_x = bx;
        float menu_y = by + UI_TYPE_ICON_SIZE + 2.0f;
        ui_rect(menu_x, menu_y, menu_w, menu_h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 0.98f);
        ui_rect(menu_x, menu_y, menu_w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
        for (int i = 0; i < PANEL_TYPE_COUNT; i++) {
            float ry = menu_y + i * row_h;
            SvgIcon *ri = icon_for_panel((PanelType)i);
            /* "[icon] Panelname" rows */
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
    /* No shadow-casting geometry source right now: this used to be the
     * octree world mesh (VERTEX_STRIDE=7, the generic format
     * gbuffer_render_shadow_map's shader assumes), gone along with the
     * rest of that code. The Phase 1 test MeshObject uses a different,
     * incompatible vertex stride (MESHOBJ_VERTEX_STRIDE, see
     * meshobject.h) and doesn't opt into shadow casting -- passing NULL
     * here is honest about that rather than silently reusing the wrong
     * stride. gbuffer_render_shadow_map itself already no-ops safely on
     * NULL/empty input. */
    gbuffer_render_shadow_map(ctx->gbuf, NULL, ctx->light_dir);
    float inv_vp[16];
    renderer_get_inverse_view_proj(ctx->renderer, inv_vp);
    gbuffer_resolve(ctx->gbuf, ctx->light_dir, ctx->sky_color, inv_vp, ctx->renderer->cam_pos);
    renderer_end_frame(ctx->renderer);

    /* gbuffer_resolve's FXAA pass just changed which program/VAO/blend
     * state is bound and cleared the depth test back on -- the 2D UI
     * pipeline needs its own state, not whatever the 3D pass left behind. */
    glDisable(GL_DEPTH_TEST);

    /* Every 2D draw below this point is the first ui_* draw call this
     * function itself has ever issued -- gbuffer_set_viewport_offset just
     * above left the real GL viewport pinned to this panel's own on-
     * screen sub-rectangle, not the full window, so a 2D quad computed
     * from ui_text_draw's full-window NDC math (u_screen_size == the
     * window, not this sub-rect) would land mis-scaled and mis-
     * positioned, not just visually "off by a few pixels" -- the exact
     * bug draw_leaf's own post-switch glViewport reset exists to prevent
     * for every OTHER panel (see its comment). That reset happens AFTER
     * this function returns, too late for anything drawn here, hence a
     * second, earlier reset needed right here. */
    glViewport(0, 0, g_ui.screen_w, g_ui.screen_h);

    /* Blender-style mode label -- top-left corner, past the type-switcher
     * icon (same offset the Outliner/Properties panel titles already use,
     * see draw_panel_outliner), drawn in the accent color while in Edit
     * Mode so the mode is legible at a glance, not just readable on close
     * inspection. */
    const char *mode_label = (ctx->editor_mode == EDITOR_MODE_EDIT) ? "Edit Mode" : "Object Mode";
    float label_x = a->x + UI_PANEL_PAD + UI_TYPE_ICON_SIZE + 6.0f;
    if (ctx->editor_mode == EDITOR_MODE_EDIT) {
        ui_text_draw(label_x, a->y + 4.0f, mode_label,
                     g_ui.font_bold, 15.0f, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 1.0f);
    } else {
        ui_text_draw(label_x, a->y + 4.0f, mode_label,
                     g_ui.font_bold, 15.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    }

    /* Modal G/S/R transform tool's live readout (see transform_op.h's
     * transform_op_hud_text via main.c) -- right after the mode label,
     * in the accent color so an in-progress Grab/Scale/Rotate is as
     * legible as Edit Mode itself. */
    if (ctx->xform_hud && ctx->xform_hud[0]) {
        float hud_x = label_x + font_text_width(g_ui.font_bold, mode_label, 15.0f) + 14.0f;
        ui_text_draw(hud_x, a->y + 4.0f, ctx->xform_hud,
                     g_ui.font_bold, 15.0f, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 1.0f);
    }
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
    /* Title only, shifted right past the type-switcher icon now sitting in
     * this same top-left corner (see draw_area_chrome) and raised to sit
     * level with it (icon top is a->y+4, not the normal a->y+UI_PANEL_PAD
     * content margin) -- the rows below it start further down, clear of
     * the icon already, so they keep the normal left margin and y. */
    ui_text_draw(x + UI_TYPE_ICON_SIZE + 6.0f, a->y + 4.0f, "Outliner", g_ui.font_bold, 15.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y += row_h + 6.0f;

    char line[128];
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
    /* Used to also list the octree World Mesh row and every connected
     * Qek Player -- both gone along with that code (see phi.md's Phase 1
     * status, "Client/server model"). The MeshObject above is the only
     * scene content that exists right now. */
}

/* Draws one PhiProp as a labeled row -- the DNA/RNA property registry
 * (phi_prop.h) is the single source of truth this reads through, not a
 * hand-picked field per struct type, so any group registered in
 * phi_prop_registry.c shows up here automatically. Read-only in the C UI
 * for this pass (see phi.md's DNA/RNA status note for why) -- the same
 * registry IS writable, just from Python (phi.prop_set) rather than a
 * click-to-edit widget here yet. */
static void draw_prop_row(float x, float *y, void *owner, const PhiProp *prop) {
    char line[128];
    if (prop->type == PHI_PROP_VEC3) {
        float v[3];
        phi_prop_get_vec3(owner, prop, v);
        snprintf(line, sizeof(line), "%s: %.2f, %.2f, %.2f", prop->display_name, v[0], v[1], v[2]);
    } else if (prop->type == PHI_PROP_BOOL) {
        float v;
        phi_prop_get_float(owner, prop, &v);
        snprintf(line, sizeof(line), "%s: %s", prop->display_name, v != 0.0f ? "yes" : "no");
    } else {
        float v;
        phi_prop_get_float(owner, prop, &v);
        snprintf(line, sizeof(line), "%s: %.2f", prop->display_name, v);
    }
    ui_text_draw(x, *y, line, g_ui.font_mono, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    *y += 20.0f;
}

static void draw_panel_properties(Area *a, const UIRenderContext *ctx) {
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    float x = a->x + UI_PANEL_PAD, y = a->y + UI_PANEL_PAD;
    ui_text_draw(x + UI_TYPE_ICON_SIZE + 6.0f, a->y + 4.0f, "Properties", g_ui.font_bold, 15.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y += 26.0f;

    if (ctx->test_obj_loaded && ctx->test_obj &&
        g_ui.selected_object_id == 4000u + (unsigned int)ctx->test_obj->id) {
        char line[96];
        snprintf(line, sizeof(line), "MeshObject #%d", ctx->test_obj->id);
        ui_text_draw(x, y, line, g_ui.font_body, 14.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        y += 22.0f;

        for (int i = 0; i < g_phi_prop_mesh_object.count; i++) {
            draw_prop_row(x, &y, ctx->test_obj, &g_phi_prop_mesh_object.props[i]);
        }
        /* Orientation isn't in the registry -- Quat (4 floats) isn't a
         * PhiPropType this pass (only scalar/bool/vec3 are), so it stays
         * a direct field read here rather than a registered prop. Real
         * quaternion editing needs real UI (an axis-angle or Euler
         * widget, not 4 raw numbers a user would ever want to type) that
         * this pass doesn't build either. */
        snprintf(line, sizeof(line), "Orientation: %.2f, %.2f, %.2f, %.2f",
                 ctx->test_obj->orientation.x, ctx->test_obj->orientation.y,
                 ctx->test_obj->orientation.z, ctx->test_obj->orientation.w);
        ui_text_draw(x, y, line, g_ui.font_mono, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 24.0f;

        /* Per-face PBR material readout (Phase 1's "PBR material assignment
         * per face"), now also property-registry-driven. Shows whichever
         * face was last ray-picked (see main.c's g_edit_face), not
         * necessarily under the cursor right now. */
        if (ctx->test_obj->hem && ctx->edit_face >= 0 &&
            ctx->edit_face < ctx->test_obj->hem->face_count &&
            !ctx->test_obj->hem->faces[ctx->edit_face].deleted) {
            HEFace *face = &ctx->test_obj->hem->faces[ctx->edit_face];
            snprintf(line, sizeof(line), "Face %d material:", ctx->edit_face);
            ui_text_draw(x, y, line, g_ui.font_body, 13.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
            y += 20.0f;
            for (int i = 0; i < g_phi_prop_heface.count; i++) {
                draw_prop_row(x + 8.0f, &y, face, &g_phi_prop_heface.props[i]);
            }
            ui_text_draw(x, y, "console: matcolor/matmetal/matrough/matemit, or phi.prop_set(...)", g_ui.font_body, 12.0f,
                         UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        } else {
            ui_text_draw(x, y, "No face selected (click a face)", g_ui.font_body, 13.0f,
                         UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        }
    } else {
        ui_text_draw(x, y, "Nothing selected", g_ui.font_body, 14.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 22.0f;
        ui_text_draw(x, y, "Click an object in the Scene or Outliner.", g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    }
}

#define UI_SCROLL_ROW_H 18.0f

/* Generic scroll-offset clamp + scrollbar-thumb rendering, shared by
 * every panel with overflowable content (Chat/Console scrollbacks,
 * the Asset Browser's item list, the Python Panel's returned rows) --
 * one geometry formula instead of four copies of the same math. Callers
 * own their own "how many rows fit"/"what's the log area's rect"
 * geometry (that part differs per panel: bottom-anchored logs vs. a
 * top-anchored list), but funnel into these two once they have top/
 * bottom/total/visible/offset. */
static int ui_clamp_scroll(int offset, int total, int visible) {
    int max_scroll = total > visible ? total - visible : 0;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    return offset;
}

/* Draws a thin track + thumb on the right edge of [top,bottom] -- only
 * called when total > visible (callers check first, same "no chrome for
 * a state that can't occur" principle as the Area menu's Join row being
 * hidden when there's no sibling).
 *
 * `anchor_bottom` picks which end offset==0 sits at, since this codebase
 * has two genuinely different kinds of scrollable content: Chat/Console
 * are bottom-anchored logs (offset==0 means "pinned to the newest line",
 * i.e. the BOTTOM of the track, matching a terminal's own convention),
 * while the Asset Browser's item list and the Python Panel's rows are
 * ordinary top-anchored lists (offset==0 means "scrolled to the top",
 * i.e. the TOP of the track, matching a normal scrollbar) -- these are
 * mirror-image thumb-position formulas, not the same math, so the flag
 * is load-bearing, not cosmetic. */
static void ui_draw_scrollbar(float bar_x, float top, float bottom, int total, int visible, int offset, int anchor_bottom) {
    float track_h = bottom - top;
    if (track_h <= 0.0f || visible <= 0) return;
    float thumb_h = track_h * ((float)visible / (float)total);
    if (thumb_h < 12.0f) thumb_h = 12.0f;
    if (thumb_h > track_h) thumb_h = track_h;
    int max_scroll = total - visible;
    float scroll_frac = max_scroll > 0 ? (float)offset / (float)max_scroll : 0.0f;
    float thumb_y = anchor_bottom
        ? bottom - thumb_h - scroll_frac * (track_h - thumb_h)
        : top + scroll_frac * (track_h - thumb_h);
    ui_rect(bar_x, top, 3.0f, track_h, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 0.5f);
    ui_rect(bar_x, thumb_y, 3.0f, thumb_h, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 0.85f);
}

/* Vertical span the scrollback has to draw in: from the panel's own top
 * padding down to just above the input prompt row + its 20px gap --
 * shared by the draw loop, the scroll clamp, and the scrollbar so none
 * of the three can disagree (same pattern chat_log_area_rect established
 * for the Chat panel). */
static void console_log_area_rect(Area *a, float *top, float *bottom) {
    *top = a->y + UI_PANEL_PAD;
    *bottom = a->y + a->h - UI_PANEL_PAD - 18.0f - 20.0f;
}

static int console_visible_row_count(Area *a) {
    float top, bottom;
    console_log_area_rect(a, &top, &bottom);
    float avail = bottom - top;
    return avail > 0.0f ? (int)(avail / UI_SCROLL_ROW_H) : 0;
}

static void draw_panel_console(Area *a, const UIRenderContext *ctx) {
    float x = a->x + UI_PANEL_PAD, y = a->y + a->h - UI_PANEL_PAD - 18.0f;
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_HD_R, UI_ZEN_PANEL_HD_G, UI_ZEN_PANEL_HD_B, 0.95f);
    if (!ctx->console) return;
    PyConsoleState *cs = ctx->console;

    /* Input row pinned to the bottom, log scrolling up from just above it
     * -- newest line closest to the input, matching normal terminal/chat
     * scrollback orientation. ">>> " matches pyconsole_submit's own echo
     * prefix (Python-REPL convention) so the live input row and its
     * echoed history read identically. */
    char prompt[CONSOLE_INPUT_LEN + 8];
    snprintf(prompt, sizeof(prompt), ">>> %s", cs->input);
    ui_text_draw(x, y, prompt, g_ui.font_mono, 13.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);

    /* Caret -- always present, since the Python panel is an always-focused
     * text input now (see console.c: keyboard input flows here every
     * frame, no open/close toggle). Standard ~1Hz blink (0.5s on / 0.5s
     * off) driven by wall-clock time rather than frame count, since
     * native's uncapped frame rate makes frame-count blink periods
     * meaningless. */
    if (fmod(phi_platform_now(), 1.0) < 0.5) {
        float caret_x = x + font_text_width(g_ui.font_mono, prompt, 13.0f) + 2.0f;
        ui_rect(caret_x, y + 1.0f, 7.0f, 14.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 0.9f);
    }
    y -= 20.0f;

    int visible = console_visible_row_count(a);
    cs->scroll_offset = ui_clamp_scroll(cs->scroll_offset, cs->log_count, visible);

    int start = cs->log_count - 1 - cs->scroll_offset;
    for (int shown = 0; shown < visible && start - shown >= 0; shown++) {
        int i = start - shown;
        ui_text_draw(x, y, cs->log[i], g_ui.font_mono, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y -= UI_SCROLL_ROW_H;
    }

    if (cs->log_count > visible) {
        float top, bottom;
        console_log_area_rect(a, &top, &bottom);
        float bar_x = a->x + a->w - UI_PANEL_PAD - 4.0f;
        ui_draw_scrollbar(bar_x, top, bottom, cs->log_count, visible, cs->scroll_offset, 1);
    }
}

/* Asset Browser panel (see phi.md's "Asset tracking and the Asset
 * Browser panel" / "Wire protocol..."). Layout constants + the geometry
 * helpers right below are shared between drawing and hit-testing
 * (hit_test_area, further down) so the two can never drift apart -- same
 * pattern type_icon_rect() already established for the per-panel type-
 * switcher icon. */
#define ASSET_BROWSER_ROW_H   20.0f
#define ASSET_BROWSER_BTN_H   22.0f
#define ASSET_BROWSER_FIELD_H 18.0f

static void asset_browser_refresh_rect(Area *a, float *x, float *y, float *w, float *h) {
    *w = 64.0f; *h = 18.0f;
    *x = a->x + a->w - UI_PANEL_PAD - *w;
    *y = a->y + 4.0f;
}

/* Fills the gap between the type-switcher icon and the Refresh button on
 * the panel's top row -- there's no separate "Asset Browser" title text
 * to share that row with (see draw_panel_asset_browser's comment on why
 * it was dropped), so the search bar gets the whole rest of the row.
 * Same y/height as the Refresh button so the two sit flush together. */
static void asset_browser_search_rect(Area *a, float *x, float *y, float *w, float *h) {
    float rx, ry, rw, rh;
    asset_browser_refresh_rect(a, &rx, &ry, &rw, &rh);
    *h = rh;
    *y = ry;
    *x = a->x + UI_PANEL_PAD + UI_TYPE_ICON_SIZE + 6.0f;
    *w = rx - 8.0f - *x;
    if (*w < 24.0f) *w = 24.0f;   /* degenerate-narrow-panel guard */
}

/* Where the list (or the edit form, when one's open) starts -- constant
 * regardless of edit state, since the edit form always sits right here
 * and pushes the list down by its own height (see asset_browser_list_top
 * below), not the other way around. */
static float asset_browser_content_top(Area *a) {
    return a->y + UI_PANEL_PAD + 26.0f;
}

/* Name field, tags field, and Save/Cancel buttons for the rename/create
 * edit form -- shown whenever ab->editing_id != AB_EDITING_NONE (see
 * asset_browser_begin_rename/begin_create), sharing one pair of buffers
 * for both since only one edit can be active at a time. Header label text
 * itself isn't a hit-testable rect, drawn directly in draw_panel_asset_
 * browser. */
static void asset_browser_edit_name_rect(Area *a, float *x, float *y, float *w, float *h) {
    *x = a->x + UI_PANEL_PAD;
    *y = asset_browser_content_top(a) + 16.0f;   /* below the "New Asset"/"Rename #N" header line */
    *w = a->w - UI_PANEL_PAD * 2.0f;
    *h = ASSET_BROWSER_FIELD_H;
}
static void asset_browser_edit_tags_rect(Area *a, float *x, float *y, float *w, float *h) {
    asset_browser_edit_name_rect(a, x, y, w, h);
    *y += ASSET_BROWSER_FIELD_H + 4.0f;
}
static void asset_browser_edit_buttons_rect(Area *a, float *save_x, float *save_y, float *save_w,
                                             float *cancel_x, float *cancel_y, float *cancel_w, float *h) {
    float tx, ty, tw, th;
    asset_browser_edit_tags_rect(a, &tx, &ty, &tw, &th);
    *h = ASSET_BROWSER_BTN_H;
    *save_y = *cancel_y = ty + th + 6.0f;
    *save_w = 70.0f; *cancel_w = 70.0f;
    *save_x = a->x + UI_PANEL_PAD;
    *cancel_x = *save_x + *save_w + 8.0f;
}
/* Total vertical space the edit form occupies, from asset_browser_
 * content_top(a) down to where the list should actually start when the
 * form is showing -- header(16) + name field + gap + tags field + gap +
 * button row + trailing gap before the list. */
static float asset_browser_edit_form_height(Area *a) {
    float bx, by, bw, bh;
    asset_browser_edit_buttons_rect(a, &bx, &by, &bw, &bx, &by, &bw, &bh);
    return (by + bh + 8.0f) - asset_browser_content_top(a);
}

static float asset_browser_list_top(Area *a, const AssetBrowserState *ab) {
    float top = asset_browser_content_top(a);
    if (ab->editing_id != AB_EDITING_NONE) top += asset_browser_edit_form_height(a);
    return top;
}

static void asset_browser_row_rect(Area *a, const AssetBrowserState *ab, int index,
                                    float *x, float *y, float *w, float *h) {
    *x = a->x + UI_PANEL_PAD;
    *y = asset_browser_list_top(a, ab) + (float)index * ASSET_BROWSER_ROW_H;
    *w = a->w - UI_PANEL_PAD * 2.0f;
    *h = ASSET_BROWSER_ROW_H;
}

static void asset_browser_action_rects(Area *a, float *load_x, float *load_y, float *load_w,
                                        float *rename_x, float *rename_y, float *rename_w,
                                        float *del_x, float *del_y, float *del_w, float *h) {
    *h = ASSET_BROWSER_BTN_H;
    *load_y = *rename_y = *del_y = a->y + a->h - UI_PANEL_PAD - *h;
    *load_w = 60.0f; *rename_w = 70.0f; *del_w = 70.0f;
    *load_x   = a->x + UI_PANEL_PAD;
    *rename_x = *load_x + *load_w + 8.0f;
    *del_x    = *rename_x + *rename_w + 8.0f;
}

/* Finds the first leaf Area of the given panel type, or NULL if none is
 * in the current layout -- same shape as find_scene_rect_r further down
 * (that one's specific to PANEL_SCENE and predates this), generalized
 * since ui_on_mouse_button now needs it for a second panel type too. */
static Area *find_area_by_type_r(Area *a, PanelType t) {
    if (a->kind == AREA_LEAF) return a->panel_type == t ? a : NULL;
    Area *found = find_area_by_type_r(a->child[0], t);
    return found ? found : find_area_by_type_r(a->child[1], t);
}

/* Pure vertical capacity -- how many rows fit above the Load/Delete
 * button row, independent of how many items actually exist or where the
 * list is scrolled to. Used directly as the scrollbar's "visible" count
 * (a constant viewport size makes for a correctly-sized thumb even on a
 * partially-filled last page -- see asset_browser_rows_to_draw below for
 * the count that's actually clamped to what's left to show). */
static int asset_browser_row_capacity(Area *a, const AssetBrowserState *ab) {
    float lx, ly, lw, rnx, rny, rnw, dx, dy, dw, bh;
    asset_browser_action_rects(a, &lx, &ly, &lw, &rnx, &rny, &rnw, &dx, &dy, &dw, &bh);
    float avail = ly - 6.0f - asset_browser_list_top(a, ab);
    return avail > 0.0f ? (int)(avail / ASSET_BROWSER_ROW_H) : 0;
}

/* How many rows actually get drawn/are clickable THIS frame: capacity,
 * further clamped by how many items remain at/after the current scroll
 * position -- shared by the draw loop and the row-click hit-test so a
 * click can never land on a row that isn't actually showing an item. */
static int asset_browser_rows_to_draw(Area *a, const AssetBrowserState *ab) {
    int capacity = asset_browser_row_capacity(a, ab);
    int remaining = ab->count - ab->list_scroll_offset;
    if (remaining < 0) remaining = 0;
    return remaining < capacity ? remaining : capacity;
}

/* Draws one text-entry field (used for the search bar, the rename/create
 * edit form's name/tags fields, and the Chat input box): background,
 * focus accent, current text or a dimmed placeholder, and a blinking
 * caret when this exact field owns focus. `is_focused` decides the
 * accent/caret; the caret's x position accounts for whatever's already
 * typed via font_text_width, same measurement draw_panel_console's own
 * caret uses. `text_y_offset` is the text/placeholder's y nudge from the
 * field's own top edge (callers pass their own constant rather than this
 * function hardcoding one) -- added so the Chat input specifically could
 * be lowered 2px per an explicit request without also shifting the Asset
 * Browser's search/name/tags fields, which share this same function and
 * were never asked to move; the caret tracks 1px above whatever
 * text_y_offset the caller chose, preserving the original 3.0/2.0
 * text/caret relationship. */
static void draw_text_field(float x, float y, float w, float h, const char *text,
                             const char *placeholder, int is_focused, float text_y_offset) {
    ui_rect(x, y, w, h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 1.0f);
    if (is_focused) {
        ui_rect(x, y, w, 2.0f, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 1.0f);
    }
    if (text[0]) {
        ui_text_draw(x + 6.0f, y + text_y_offset, text, g_ui.font_body, 12.0f,
                     UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    } else if (!is_focused && placeholder) {
        ui_text_draw(x + 6.0f, y + text_y_offset, placeholder, g_ui.font_body, 12.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    }
    if (is_focused && fmod(phi_platform_now(), 1.0) < 0.5) {
        float caret_x = x + 6.0f + (text[0] ? font_text_width(g_ui.font_body, text, 12.0f) : 0.0f);
        ui_rect(caret_x, y + text_y_offset - 1.0f, 6.0f, h - 4.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 0.9f);
    }
}

static void draw_panel_asset_browser(Area *a, const UIRenderContext *ctx) {
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    /* No separate "Asset Browser" title text on this panel -- the type-
     * switcher icon in the top-left corner (draw_area_chrome, same as
     * every other panel) already identifies it, and the search bar needs
     * that whole row to sit to the left of Refresh as asked for, rather
     * than being squeezed in next to a redundant label. */

    float rx, ry, rw, rh;
    asset_browser_refresh_rect(a, &rx, &ry, &rw, &rh);
    ui_rect(rx, ry, rw, rh, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 1.0f);
    ui_text_draw(rx + 6.0f, ry + 3.0f, "Refresh", g_ui.font_body, 11.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);

    if (!ctx->asset_browser) return;
    AssetBrowserState *ab = ctx->asset_browser;

    float sx, sy, sw, sh;
    asset_browser_search_rect(a, &sx, &sy, &sw, &sh);
    draw_text_field(sx, sy, sw, sh, ab->search, "Search...", ab->focus == AB_FOCUS_SEARCH, 3.0f);

    if (ab->editing_id != AB_EDITING_NONE) {
        float content_top = asset_browser_content_top(a);
        char header[48];
        if (ab->editing_id == AB_EDITING_NEW) snprintf(header, sizeof(header), "New Asset");
        else snprintf(header, sizeof(header), "Rename #%d", ab->editing_id);
        ui_text_draw(a->x + UI_PANEL_PAD, content_top, header, g_ui.font_body, 12.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);

        float nx, ny, nw, nh;
        asset_browser_edit_name_rect(a, &nx, &ny, &nw, &nh);
        draw_text_field(nx, ny, nw, nh, ab->edit_name, "Name...", ab->focus == AB_FOCUS_EDIT_NAME, 3.0f);

        float tx, ty, tw, th;
        asset_browser_edit_tags_rect(a, &tx, &ty, &tw, &th);
        draw_text_field(tx, ty, tw, th, ab->edit_tags, "tags, comma, separated", ab->focus == AB_FOCUS_EDIT_TAGS, 3.0f);

        float bsx, bsy, bsw, bcx, bcy, bcw, bbh;
        asset_browser_edit_buttons_rect(a, &bsx, &bsy, &bsw, &bcx, &bcy, &bcw, &bbh);
        ui_rect(bsx, bsy, bsw, bbh, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 1.0f);
        ui_text_draw(bsx + 8.0f, bsy + 4.0f, ab->editing_id == AB_EDITING_NEW ? "Create" : "Save",
                     g_ui.font_body, 12.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        ui_rect(bcx, bcy, bcw, bbh, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 1.0f);
        ui_text_draw(bcx + 8.0f, bcy + 4.0f, "Cancel", g_ui.font_body, 12.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    }

    if (ab->count == 0) {
        ui_text_draw(a->x + UI_PANEL_PAD, asset_browser_list_top(a, ab), "No assets indexed. Click Refresh.",
                     g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
    }
    int capacity = asset_browser_row_capacity(a, ab);
    /* Defensive re-clamp every frame, same reasoning as Chat/Console's own
     * scroll_offset clamps -- the list can shrink (a delete, or a new
     * search/refresh reply) out from under a stale scroll position. */
    ab->list_scroll_offset = ui_clamp_scroll(ab->list_scroll_offset, ab->count, capacity);
    int rows = asset_browser_rows_to_draw(a, ab);
    for (int row = 0; row < rows; row++) {
        int i = row + ab->list_scroll_offset;   /* absolute index into ab->items[] */
        float x, y, w, h;
        asset_browser_row_rect(a, ab, row, &x, &y, &w, &h);
        if (i == ab->selected) {
            ui_rect(x, y - 2.0f, w, h, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 0.35f);
        }
        char line[160];
        const AssetSummary *as = &ab->items[i];
        if (as->tags[0])
            snprintf(line, sizeof(line), "#%u  %s  [%s]", as->id, as->name, as->tags);
        else
            snprintf(line, sizeof(line), "#%u  %s", as->id, as->name);
        ui_text_draw(x + 4.0f, y, line, g_ui.font_body, 13.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    }
    if (ab->count > capacity) {
        float top = asset_browser_list_top(a, ab);
        float lx2, ly2, lw2, rnx2, rny2, rnw2, dx2, dy2, dw2, bh2;
        asset_browser_action_rects(a, &lx2, &ly2, &lw2, &rnx2, &rny2, &rnw2, &dx2, &dy2, &dw2, &bh2);
        float bar_x = a->x + a->w - 6.0f;
        /* Top-anchored: offset==0 shows the top of the list, matching a
         * normal scrollbar's own convention (see ui_draw_scrollbar's own
         * comment on why this differs from Chat/Console's bottom anchor). */
        ui_draw_scrollbar(bar_x, top, ly2 - 6.0f, ab->count, capacity, ab->list_scroll_offset, 0);
    }

    float lx, ly, lw, rnx, rny, rnw, dx, dy, dw, bh;
    asset_browser_action_rects(a, &lx, &ly, &lw, &rnx, &rny, &rnw, &dx, &dy, &dw, &bh);
    int have_sel = ab->selected >= 0 && ab->selected < ab->count;
    float dim = have_sel ? 1.0f : 0.4f;
    ui_rect(lx, ly, lw, bh, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, dim);
    ui_text_draw(lx + 8.0f, ly + 4.0f, "Load", g_ui.font_body, 12.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, dim);
    ui_rect(rnx, rny, rnw, bh, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, dim);
    ui_text_draw(rnx + 8.0f, rny + 4.0f, "Rename", g_ui.font_body, 12.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, dim);
    ui_rect(dx, dy, dw, bh, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, dim);
    ui_text_draw(dx + 8.0f, dy + 4.0f, "Delete", g_ui.font_body, 12.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, dim);
}

#define CHAT_ROW_H UI_SCROLL_ROW_H

/* Input row pinned to the bottom, same geometry shape asset_browser's
 * field rects use (a dedicated function shared between drawing and
 * hit-testing, see hit_test_area/ui_on_mouse_button below, so the two can
 * never disagree about where the box actually is). */
static void chat_input_rect(Area *a, float *x, float *y, float *w, float *h) {
    *h = 24.0f;
    *w = a->w - UI_PANEL_PAD * 2.0f;
    *x = a->x + UI_PANEL_PAD;
    *y = a->y + a->h - UI_PANEL_PAD - *h;
}

/* Vertical span the scrollback actually has to draw in: from just below
 * the "Chat" title row down to a real 20px clearance above the input box
 * (matching draw_panel_console's own input-to-first-log-line gap) --
 * previously only 8px, which let the bottom of the newest line's glyphs
 * clip into the input box's top edge, a real reported bug. Shared by the
 * draw loop, the wheel-scroll clamp, and the scrollbar thumb geometry so
 * none of the three can disagree about how much room there is. */
static void chat_log_area_rect(Area *a, float *top, float *bottom) {
    float ix, iy, iw, ih;
    chat_input_rect(a, &ix, &iy, &iw, &ih);
    *top = a->y + 30.0f;
    *bottom = iy - 20.0f;
}

/* How many scrollback rows actually fit right now -- reserve_rows lets
 * the caller carve out space for the "Claude is thinking..." row when
 * it's showing, so that indicator doesn't silently steal a row's worth
 * of space nobody accounted for. */
static int chat_visible_row_count(Area *a, int reserve_rows) {
    float top, bottom;
    chat_log_area_rect(a, &top, &bottom);
    float avail = (bottom - top) - (float)reserve_rows * CHAT_ROW_H;
    return avail > 0.0f ? (int)(avail / CHAT_ROW_H) : 0;
}

#define CHAT_VISUAL_ROW_LEN   128
#define CHAT_MAX_VISUAL_ROWS  256
#define CHAT_MAX_WRAP_PER_MSG 24

/* Greedy word-wrap: breaks `text` into rows no wider than avail_w
 * (measured via font_text_width at the given font/size), splitting on
 * spaces. The very first word attempted on a row is always taken even if
 * it alone exceeds avail_w (no mid-word hard-breaking -- a rare edge
 * case for real chat text, not worth the complexity here), so this can
 * never get stuck making zero progress. An empty `text` still produces
 * one empty row rather than zero, so a deliberate blank scrollback line
 * (see chat_init's preamble spacing) still occupies real vertical space
 * instead of silently vanishing. Returns the row count written (capped
 * at max_rows). */
static int wrap_text(const char *text, float avail_w, const Font *font, float size,
                      char out[][CHAT_VISUAL_ROW_LEN], int max_rows) {
    if (max_rows <= 0) return 0;
    if (!text[0]) { out[0][0] = 0; return 1; }

    int n = 0;
    const char *p = text;
    while (*p == ' ') p++;
    while (*p && n < max_rows) {
        const char *row_start = p;
        const char *cursor = p;
        const char *last_good_end = row_start;
        for (;;) {
            const char *word_start = cursor;
            while (*word_start == ' ') word_start++;
            if (!*word_start) { last_good_end = word_start; cursor = word_start; break; }
            const char *word_end = word_start;
            while (*word_end && *word_end != ' ') word_end++;

            size_t len = (size_t)(word_end - row_start);
            if (len >= CHAT_VISUAL_ROW_LEN) len = CHAT_VISUAL_ROW_LEN - 1;
            char candidate[CHAT_VISUAL_ROW_LEN];
            memcpy(candidate, row_start, len);
            candidate[len] = 0;

            if (word_start == row_start || font_text_width(font, candidate, size) <= avail_w) {
                last_good_end = word_end;
                cursor = word_end;
                if (!*word_end) break;
            } else {
                break;
            }
        }
        size_t row_len = (size_t)(last_good_end - row_start);
        if (row_len >= CHAT_VISUAL_ROW_LEN) row_len = CHAT_VISUAL_ROW_LEN - 1;
        memcpy(out[n], row_start, row_len);
        out[n][row_len] = 0;
        n++;
        p = last_good_end;
        while (*p == ' ') p++;
    }
    return n;
}

typedef struct {
    char text[CHAT_VISUAL_ROW_LEN];
    /* > 0 = draw text[0..bold_prefix_len) in g_ui.font_bold, then
     * text[bold_prefix_len..] (starting at the ": ") in the normal body
     * font -- see ChatLogLine's own comment on why only a wrapped row's
     * first line of a real message-start entry ever gets this. */
    int  bold_prefix_len;
} ChatVisualRow;

/* Rebuilds the flat, currently-visible-width-wrapped row list from
 * cs->log[] every frame (never cached) -- the only way wrapping can
 * genuinely be "responsive to pane resizes" the way it was asked to be,
 * since a cached pre-wrapped version would go stale the moment the panel
 * (or the window) resizes. Returns the row count written (capped at
 * max_out); indices run oldest-to-newest, same order as cs->log[]
 * itself, so all the existing scroll_offset/visible/scrollbar math below
 * can operate on this exactly the way it operated on cs->log[] directly
 * before word-wrap existed. */
static int chat_build_visual_rows(const ChatState *cs, float avail_w, ChatVisualRow *out, int max_out) {
    int n = 0;
    for (int e = 0; e < cs->log_count && n < max_out; e++) {
        const ChatLogLine *entry = &cs->log[e];
        char wrapped[CHAT_MAX_WRAP_PER_MSG][CHAT_VISUAL_ROW_LEN];
        int wn = wrap_text(entry->text, avail_w, g_ui.font_body, 13.0f, wrapped, CHAT_MAX_WRAP_PER_MSG);
        for (int w = 0; w < wn && n < max_out; w++) {
            ChatVisualRow *row = &out[n++];
            strncpy(row->text, wrapped[w], CHAT_VISUAL_ROW_LEN - 1);
            row->text[CHAT_VISUAL_ROW_LEN - 1] = 0;
            row->bold_prefix_len = 0;
            if (w == 0 && entry->is_msg_start) {
                const char *colon = strstr(row->text, ": ");
                if (colon) row->bold_prefix_len = (int)(colon - row->text);
            }
        }
    }
    return n;
}

/* Chat panel -- a real text field wired to a real server-side Anthropic
 * tool-use loop (see chat.h / server/anthropic_client.py / phi.md's
 * "Where AI fits"), not the placeholder shell this used to be. Click the
 * input box to focus it (see ui_on_mouse_button's click-to-focus/
 * click-away-blur handling below, mirroring the Asset Browser's fields
 * exactly), type, Enter to send. reuses draw_text_field, the same
 * focus-accent/placeholder/caret widget the Asset Browser's search/name/
 * tags fields already use, rather than a bespoke input box -- lowered
 * 2px from that shared default via draw_text_field's text_y_offset
 * parameter, per an explicit request scoped to just this panel. */
static void draw_panel_chat(Area *a, const UIRenderContext *ctx) {
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    float x = a->x + UI_PANEL_PAD;
    ui_text_draw(x + UI_TYPE_ICON_SIZE + 6.0f, a->y + 4.0f, "Chat", g_ui.font_bold, 15.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    if (!ctx->chat) return;
    ChatState *cs = ctx->chat;

    float ix, iy, iw, ih;
    chat_input_rect(a, &ix, &iy, &iw, &ih);
    draw_text_field(ix, iy, iw, ih, cs->input, "Type a message...", cs->focus == CHAT_FOCUS_INPUT, 5.0f);

    /* Symmetric margins: text starts UI_PANEL_PAD in from the left, so it
     * wraps UI_PANEL_PAD before the right edge too -- per an explicit
     * request that the two match, rather than wrapping flush against the
     * panel edge or the scrollbar. Recomputed from `a->w` every frame
     * (never cached), which is what actually makes this resize-
     * responsive. */
    float avail_w = a->w - UI_PANEL_PAD * 2.0f;
    static ChatVisualRow rows[CHAT_MAX_VISUAL_ROWS];   /* function-local static: avoids a large per-frame stack allocation; single-threaded rendering, one Chat panel, no reentrancy concern */
    int total_rows = chat_build_visual_rows(cs, avail_w, rows, CHAT_MAX_VISUAL_ROWS);

    int reserve = cs->waiting_for_reply ? 1 : 0;
    int visible = chat_visible_row_count(a, reserve);
    /* Defensive re-clamp every frame (not just on wheel events) -- the
     * row count can change under a stale scroll_offset for several
     * reasons now (a new reply, a resize changing the wrap, a panel
     * resize/join), so this can't only be enforced at the moment of
     * scrolling. */
    cs->scroll_offset = ui_clamp_scroll(cs->scroll_offset, total_rows, visible);

    float y = iy - 20.0f;
    if (cs->waiting_for_reply) {
        ui_text_draw(x, y, "Claude is thinking...", g_ui.font_body, 12.0f, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 1.0f);
        y -= CHAT_ROW_H;
    }

    /* Newest-closest-to-input, same scrollback orientation draw_panel_
     * console uses -- `start` is the bottom-most (newest-of-the-visible-
     * window) row index; scroll_offset > 0 walks it back into history. */
    int start = total_rows - 1 - cs->scroll_offset;
    for (int shown = 0; shown < visible && start - shown >= 0; shown++) {
        int i = start - shown;
        const ChatVisualRow *row = &rows[i];
        if (row->bold_prefix_len > 0) {
            char prefix[CHAT_VISUAL_ROW_LEN];
            int plen = row->bold_prefix_len;
            if (plen >= CHAT_VISUAL_ROW_LEN) plen = CHAT_VISUAL_ROW_LEN - 1;
            memcpy(prefix, row->text, (size_t)plen);
            prefix[plen] = 0;
            ui_text_draw(x, y, prefix, g_ui.font_bold, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
            float rest_x = x + font_text_width(g_ui.font_bold, prefix, 13.0f);
            ui_text_draw(rest_x, y, row->text + plen, g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        } else {
            ui_text_draw(x, y, row->text, g_ui.font_body, 13.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        }
        y -= CHAT_ROW_H;
    }

    /* Scrollbar -- bottom-anchored (offset==0 pins to the newest line at
     * the bottom of the track), wheel-only for now (not draggable), a
     * known first-pass scope cut, not an oversight. */
    if (total_rows > visible) {
        float top, bottom;
        chat_log_area_rect(a, &top, &bottom);
        bottom -= (float)reserve * CHAT_ROW_H;
        float bar_x = a->x + a->w - UI_PANEL_PAD - 4.0f;
        ui_draw_scrollbar(bar_x, top, bottom, total_rows, visible, cs->scroll_offset, 1);
    }
}

static void draw_panel_stub(Area *a, const char *name) {
    ui_text_draw(a->x + UI_PANEL_PAD + UI_TYPE_ICON_SIZE + 6.0f, a->y + 4.0f, name, g_ui.font_bold, 15.0f,
                 UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    ui_text_draw(a->x + UI_PANEL_PAD, a->y + UI_PANEL_PAD + 24.0f, "Not implemented yet.",
                 g_ui.font_body, 14.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
}

/* Python Panel (see phi.md's "Python-extensible panels" / mp_port.h) --
 * renders whatever the FIRST @phi.panel-registered class's draw(ctx)
 * returned (a plain list of text-row strings, see phi_mp_draw_panel's own
 * comment on why this pass doesn't build real interactive Python-defined
 * widgets yet). Which panel to show when more than one is registered
 * isn't decided by anything yet either -- always panel 0 -- a known,
 * documented simplification, not an oversight. ctx isn't used: everything
 * this needs (phi_mp_panel_count/name/draw_panel) is process-global state
 * in mp_port.c, the same way console.c's ConsoleState-free functions
 * would be if console_append() didn't need a specific target instance. */
static void draw_panel_python(Area *a, const UIRenderContext *ctx) {
    (void)ctx;
    ui_rect(a->x, a->y, a->w, a->h, UI_ZEN_PANEL_BG_R, UI_ZEN_PANEL_BG_G, UI_ZEN_PANEL_BG_B, UI_ZEN_PANEL_BG_A);
    float x = a->x + UI_PANEL_PAD, y = a->y + UI_PANEL_PAD;

    int count = phi_mp_panel_count();
    if (count == 0) {
        ui_text_draw(x + UI_TYPE_ICON_SIZE + 6.0f, a->y + 4.0f, "Python Panel", g_ui.font_bold, 15.0f,
                     UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        y += 26.0f;
        ui_text_draw(x, y, "No @phi.panel registered yet.", g_ui.font_body, 13.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 20.0f;
        ui_text_draw(x, y, "Define one from the Python Console, e.g.:", g_ui.font_body, 13.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += 20.0f;
        ui_text_draw(x, y, "@phi.panel('My Panel')", g_ui.font_mono, 12.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        return;
    }

    ui_text_draw(x + UI_TYPE_ICON_SIZE + 6.0f, a->y + 4.0f, phi_mp_panel_name(0), g_ui.font_bold, 15.0f,
                 UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
    y += 26.0f;

    char lines[16][256];
    int n = phi_mp_draw_panel(0, lines, 16);
    if (n < 0) {
        ui_text_draw(x, y, "draw() raised -- see the Console log", g_ui.font_body, 13.0f,
                     0.9f, 0.35f, 0.3f, 1.0f);
        y += 20.0f;
        char first_line[96];
        const char *err = phi_mp_last_captured_output();
        const char *nl = strchr(err, '\n');
        size_t n_copy = nl ? (size_t)(nl - err) : strlen(err);
        if (n_copy >= sizeof(first_line)) n_copy = sizeof(first_line) - 1;
        memcpy(first_line, err, n_copy);
        first_line[n_copy] = 0;
        ui_text_draw(x, y, first_line, g_ui.font_mono, 12.0f, UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        return;
    }
    float list_top = y;
    float list_bottom = a->y + a->h - UI_PANEL_PAD;
    int capacity = list_bottom > list_top ? (int)((list_bottom - list_top) / UI_SCROLL_ROW_H) : 0;
    g_ui.python_panel_scroll = ui_clamp_scroll(g_ui.python_panel_scroll, n, capacity);
    int remaining = n - g_ui.python_panel_scroll;
    if (remaining < 0) remaining = 0;
    int rows = remaining < capacity ? remaining : capacity;
    for (int row = 0; row < rows; row++) {
        ui_text_draw(x, y, lines[row + g_ui.python_panel_scroll], g_ui.font_mono, 13.0f,
                     UI_ZEN_TEXT_DIM_R, UI_ZEN_TEXT_DIM_G, UI_ZEN_TEXT_DIM_B, 1.0f);
        y += UI_SCROLL_ROW_H;
    }
    if (n > capacity) {
        float bar_x = a->x + a->w - 6.0f;
        /* Top-anchored, same convention as the Asset Browser's list. */
        ui_draw_scrollbar(bar_x, list_top, list_bottom, n, capacity, g_ui.python_panel_scroll, 0);
    }
}

static void draw_leaf(Area *a, const UIRenderContext *ctx) {
    switch (a->panel_type) {
        case PANEL_SCENE:      draw_panel_scene(a, ctx); break;
        case PANEL_OUTLINER:   draw_panel_outliner(a, ctx); break;    /* fills its own background, see above */
        case PANEL_PROPERTIES: draw_panel_properties(a, ctx); break;  /* ditto */
        case PANEL_CONSOLE:    draw_panel_console(a, ctx); break;
        case PANEL_CHAT:       draw_panel_chat(a, ctx); break;        /* ditto */
        case PANEL_ASSET_BROWSER: draw_panel_asset_browser(a, ctx); break;
        case PANEL_PYTHON:     draw_panel_python(a, ctx); break;
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

#define CTX_MENU_MAX_ROWS 9

/* Builds this frame's Scene right-click context-menu row set -- single
 * source of truth for both the draw pass (ui_render) and the hit-test pass
 * (ui_on_mouse_button), which previously hand-duplicated an identical
 * items[] (and, in the hit-test copy only, actions[]) array that could
 * silently drift out of sync. Row set depends on ctx->editor_mode (see
 * EditorMode's own comment): Object mode shows whole-object operations
 * plus "Enter Edit Mode" (only when a mesh is actually selected -- omitted
 * entirely rather than shown-then-rejected, same convention the Area
 * menu's can_join check above already uses for "Join with Sibling"); Edit
 * mode shows the mesh-editing ops plus "Exit Edit Mode". Returns the row
 * count; fills items/actions up to CTX_MENU_MAX_ROWS. */
static int build_ctx_menu_rows(const UIRenderContext *ctx, const char *items[CTX_MENU_MAX_ROWS],
                                CtxMenuAction actions[CTX_MENU_MAX_ROWS]) {
    int n = 0;
    if (ctx->editor_mode == EDITOR_MODE_EDIT) {
        items[n] = "Extrude Face";    actions[n++] = CTX_ACTION_EXTRUDE_FACE;
        items[n] = "Inset Face";      actions[n++] = CTX_ACTION_INSET_FACE;
        items[n] = "Loop Cut";        actions[n++] = CTX_ACTION_LOOP_CUT;
        items[n] = "Exit Edit Mode";  actions[n++] = CTX_ACTION_TOGGLE_EDIT_MODE;
    } else {
        items[n] = "Add > Mesh Object";  actions[n++] = CTX_ACTION_ADD_MESH;
        items[n] = "Delete";             actions[n++] = CTX_ACTION_DELETE;
        items[n] = "Frame Selected";     actions[n++] = CTX_ACTION_FRAME_SELECTED;
        items[n] = "Frame All";          actions[n++] = CTX_ACTION_FRAME_ALL;
        items[n] = "Deselect All";       actions[n++] = CTX_ACTION_DESELECT_ALL;
        items[n] = "Fracture (Voronoi)"; actions[n++] = CTX_ACTION_FRACTURE;
        items[n] = "Save as Asset";      actions[n++] = CTX_ACTION_SAVE_AS_ASSET;
        items[n] = "Enable Physics";     actions[n++] = CTX_ACTION_ENABLE_PHYSICS;
        int mesh_selected = ctx->test_obj_loaded && ctx->test_obj &&
            ui_get_selected_object() == 4000u + (unsigned int)ctx->test_obj->id;
        if (mesh_selected) {
            items[n] = "Enter Edit Mode"; actions[n++] = CTX_ACTION_TOGGLE_EDIT_MODE;
        }
    }
    return n;
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

    /* Border resize highlight -- actively-dragged border wins over a
     * merely-hovered one if somehow both are set (shouldn't happen, but
     * dragging is the more truthful state to show if it does). No OS
     * cursor-shape change (e.g. a resize cursor icon) -- this codebase has
     * no cursor-shape API wired up on any platform yet, flagged rather
     * than silently assumed; this accent line is the whole affordance for
     * now. */
    {
        Area *hl = g_ui.resizing_area ? g_ui.resizing_area : g_ui.hover_border;
        if (hl) {
            if (hl->kind == AREA_SPLIT_H) {
                float bx = hl->x + hl->w * hl->split;
                ui_rect(bx - 1.0f, hl->y, 2.0f, hl->h, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 0.9f);
            } else {
                float by = hl->y + hl->h * hl->split;
                ui_rect(hl->x, by - 1.0f, hl->w, 2.0f, UI_ZEN_ACCENT_R, UI_ZEN_ACCENT_G, UI_ZEN_ACCENT_B, 0.9f);
            }
        }
    }

    if (g_ui.area_menu_open && g_ui.area_menu_target) {
        float menu_w = 170.0f, row_h = 24.0f;
        int can_join = area_tree_find_parent(g_ui.root, g_ui.area_menu_target) != NULL;
        static const char *items_full[] = { "Split Horizontal", "Split Vertical", "Join with Sibling" };
        int n = can_join ? 3 : 2;   /* root has no sibling to join with -- just don't show the row rather than show-then-reject-it */
        float menu_h = row_h * n;
        ui_rect(g_ui.area_menu_x, g_ui.area_menu_y, menu_w, menu_h, UI_ZEN_WIDGET_R, UI_ZEN_WIDGET_G, UI_ZEN_WIDGET_B, 0.98f);
        ui_rect(g_ui.area_menu_x, g_ui.area_menu_y, menu_w, 1.0f, UI_ZEN_BORDER_R, UI_ZEN_BORDER_G, UI_ZEN_BORDER_B, 1.0f);
        for (int i = 0; i < n; i++) {
            ui_text_draw(g_ui.area_menu_x + 10.0f, g_ui.area_menu_y + i * row_h + 4.0f, items_full[i],
                         g_ui.font_body, 13.0f, UI_ZEN_TEXT_R, UI_ZEN_TEXT_G, UI_ZEN_TEXT_B, 1.0f);
        }
    }

    if (g_ui.ctx_menu_open) {
        float menu_w = 180.0f, row_h = 24.0f;
        const char *items[CTX_MENU_MAX_ROWS];
        CtxMenuAction actions[CTX_MENU_MAX_ROWS];
        int n = build_ctx_menu_rows(ctx, items, actions);
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
    float bx, by;
    type_icon_rect(a, &bx, &by);
    if (button == 0 && pressed && point_in_rect((float)x, (float)y, bx, by, UI_TYPE_ICON_SIZE, UI_TYPE_ICON_SIZE)) {
        a->type_menu_open = !a->type_menu_open;
        return 1;
    }
    /* Right-click on the same icon opens the Blender-style Area menu
     * (Split Horizontal/Vertical, Join with Sibling) -- the real corner-
     * drag gesture Blender itself uses is a menu-driven equivalent of,
     * not a pixel-for-pixel replication (see phi.md's note on this
     * tradeoff). Same trigger spot, different button, as the type
     * dropdown right above. */
    if (button == 1 && pressed && point_in_rect((float)x, (float)y, bx, by, UI_TYPE_ICON_SIZE, UI_TYPE_ICON_SIZE)) {
        g_ui.area_menu_open = 1;
        g_ui.area_menu_target = a;
        g_ui.area_menu_x = (float)x;
        g_ui.area_menu_y = (float)y;
        a->type_menu_open = 0;   /* the two menus are mutually exclusive on the same icon */
        return 1;
    }
    if (a->type_menu_open) {
        float menu_w = type_menu_width(), row_h = 24.0f;
        float menu_h = row_h * PANEL_TYPE_COUNT;
        float menu_x = bx;  /* mirrors draw_area_chrome's now top-left-aligned menu spawn */
        float menu_y = by + UI_TYPE_ICON_SIZE + 2.0f;
        if (button == 0 && pressed) {
            if (point_in_rect((float)x, (float)y, menu_x, menu_y, menu_w, menu_h)) {
                int row = (int)(((float)y - menu_y) / row_h);
                if (row >= 0 && row < PANEL_TYPE_COUNT) {
                    /* Switching this leaf away from PANEL_ASSET_BROWSER
                     * makes its search/edit fields disappear from the
                     * layout -- drop focus (not editing_id: an in-progress
                     * rename/create is a draft, not discarded just because
                     * its panel got hidden) or the console would stay
                     * silently locked out with no visible box left to
                     * click to blur it. */
                    if (a->panel_type == PANEL_ASSET_BROWSER && ctx->asset_browser)
                        ctx->asset_browser->focus = AB_FOCUS_NONE;
                    a->panel_type = (PanelType)row;
                }
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
    if (a->panel_type == PANEL_ASSET_BROWSER && button == 0 && pressed && ctx->asset_browser &&
        point_in_rect((float)x, (float)y, a->x, a->y, a->w, a->h)) {
        AssetBrowserState *ab = ctx->asset_browser;

        float rx, ry, rw, rh;
        asset_browser_refresh_rect(a, &rx, &ry, &rw, &rh);
        if (point_in_rect((float)x, (float)y, rx, ry, rw, rh)) {
            ab->refresh_requested = 1;
            return 1;
        }

        float sx, sy, sw, sh;
        asset_browser_search_rect(a, &sx, &sy, &sw, &sh);
        if (point_in_rect((float)x, (float)y, sx, sy, sw, sh)) {
            ab->focus = AB_FOCUS_SEARCH;
            return 1;
        }

        if (ab->editing_id != AB_EDITING_NONE) {
            float nx, ny, nw, nh;
            asset_browser_edit_name_rect(a, &nx, &ny, &nw, &nh);
            if (point_in_rect((float)x, (float)y, nx, ny, nw, nh)) {
                ab->focus = AB_FOCUS_EDIT_NAME;
                return 1;
            }
            float tx, ty, tw, th;
            asset_browser_edit_tags_rect(a, &tx, &ty, &tw, &th);
            if (point_in_rect((float)x, (float)y, tx, ty, tw, th)) {
                ab->focus = AB_FOCUS_EDIT_TAGS;
                return 1;
            }
            float bsx, bsy, bsw, bcx, bcy, bcw, bbh;
            asset_browser_edit_buttons_rect(a, &bsx, &bsy, &bsw, &bcx, &bcy, &bcw, &bbh);
            if (point_in_rect((float)x, (float)y, bsx, bsy, bsw, bbh)) {
                if (ab->editing_id == AB_EDITING_NEW) ab->create_requested = 1;
                else ab->update_requested = 1;
                return 1;
            }
            if (point_in_rect((float)x, (float)y, bcx, bcy, bcw, bbh)) {
                asset_browser_cancel_edit(ab);
                return 1;
            }
        }

        float list_top = asset_browser_list_top(a, ab);
        if ((float)y >= list_top - 2.0f) {
            int rows = asset_browser_rows_to_draw(a, ab);
            int row = (int)(((float)y - (list_top - 2.0f)) / ASSET_BROWSER_ROW_H);
            if (row >= 0 && row < rows) {
                ab->selected = row + ab->list_scroll_offset;   /* absolute item index, see draw_panel_asset_browser */
                return 1;
            }
        }

        float lx, ly, lw, rnx, rny, rnw, dx, dy, dw, bh;
        asset_browser_action_rects(a, &lx, &ly, &lw, &rnx, &rny, &rnw, &dx, &dy, &dw, &bh);
        if (ab->selected >= 0 && ab->selected < ab->count) {
            if (point_in_rect((float)x, (float)y, lx, ly, lw, bh)) {
                ab->load_requested = 1;
                ab->load_requested_id = ab->items[ab->selected].id;
                return 1;
            }
            if (point_in_rect((float)x, (float)y, rnx, rny, rnw, bh)) {
                asset_browser_begin_rename(ab, ab->selected);
                return 1;
            }
            if (point_in_rect((float)x, (float)y, dx, dy, dw, bh)) {
                ab->delete_requested = 1;
                ab->delete_requested_id = ab->items[ab->selected].id;
                return 1;
            }
        }
        return 1;
    }
    if (a->panel_type == PANEL_CHAT && button == 0 && pressed && ctx->chat &&
        point_in_rect((float)x, (float)y, a->x, a->y, a->w, a->h)) {
        float ix, iy, iw, ih;
        chat_input_rect(a, &ix, &iy, &iw, &ih);
        if (point_in_rect((float)x, (float)y, ix, iy, iw, ih)) {
            ctx->chat->focus = CHAT_FOCUS_INPUT;
        }
        return 1;
    }
    return 0;
}

int ui_on_mouse_button(int x, int y, int button, int pressed, const UIRenderContext *ctx) {
    if (g_ui.ctx_menu_open) {
        float menu_w = 180.0f, row_h = 24.0f;
        const char *items[CTX_MENU_MAX_ROWS];
        CtxMenuAction actions[CTX_MENU_MAX_ROWS];
        int n = build_ctx_menu_rows(ctx, items, actions);
        float menu_h = row_h * n;
        if (button == 0 && pressed) {
            if (point_in_rect((float)x, (float)y, g_ui.ctx_menu_x, g_ui.ctx_menu_y, menu_w, menu_h)) {
                int row = (int)(((float)y - g_ui.ctx_menu_y) / row_h);
                if (row >= 0 && row < n) {
                    printf("[ui] context menu: '%s'\n", items[row]);
                    g_ui.ctx_menu_pending_action = actions[row];
                }
            }
            g_ui.ctx_menu_open = 0;
            return 1;
        }
        if (button == 1 && pressed) { g_ui.ctx_menu_open = 0; return 1; }
    }

    if (g_ui.area_menu_open && g_ui.area_menu_target) {
        float menu_w = 170.0f, row_h = 24.0f;
        int can_join = area_tree_find_parent(g_ui.root, g_ui.area_menu_target) != NULL;
        int n = can_join ? 3 : 2;
        float menu_h = row_h * n;
        if (button == 0 && pressed) {
            if (point_in_rect((float)x, (float)y, g_ui.area_menu_x, g_ui.area_menu_y, menu_w, menu_h)) {
                int row = (int)(((float)y - g_ui.area_menu_y) / row_h);
                Area *target = g_ui.area_menu_target;
                if (row == 0) {
                    area_tree_split(target, AREA_SPLIT_H);
                } else if (row == 1) {
                    area_tree_split(target, AREA_SPLIT_V);
                } else if (row == 2 && can_join) {
                    area_tree_join_with_sibling(g_ui.root, target);
                }
            }
            g_ui.area_menu_open = 0;
            g_ui.area_menu_target = NULL;
            return 1;
        }
        if (button == 1 && pressed) { g_ui.area_menu_open = 0; g_ui.area_menu_target = NULL; return 1; }
    }

    /* Asset Browser text field blur: any click that doesn't land inside
     * whichever field (search/edit_name/edit_tags) currently owns focus
     * drops that focus -- same click-away-dismisses convention the
     * type-switcher dropdown and context menu above already use, just for
     * a text field instead of a dropdown. Checked before anything else
     * claims the click (including clicks inside the chrome strip below,
     * or in a completely different panel), so a click that goes on to do
     * something else still blurs first -- there's no scenario where a
     * real click should leave a stale caret blinking in a box that no
     * longer has focus. Blurring never touches editing_id (see the
     * type-switcher-away comment above) -- an in-progress rename/create
     * stays open, just not receiving keystrokes, until Save/Cancel. */
    if (button == 0 && pressed && ctx->asset_browser && ctx->asset_browser->focus != AB_FOCUS_NONE) {
        AssetBrowserState *ab = ctx->asset_browser;
        Area *ab_area = g_ui.root ? find_area_by_type_r(g_ui.root, PANEL_ASSET_BROWSER) : NULL;
        int inside = 0;
        if (ab_area) {
            float fx, fy, fw, fh;
            switch (ab->focus) {
                case AB_FOCUS_SEARCH:     asset_browser_search_rect(ab_area, &fx, &fy, &fw, &fh); inside = point_in_rect((float)x, (float)y, fx, fy, fw, fh); break;
                case AB_FOCUS_EDIT_NAME:  asset_browser_edit_name_rect(ab_area, &fx, &fy, &fw, &fh); inside = point_in_rect((float)x, (float)y, fx, fy, fw, fh); break;
                case AB_FOCUS_EDIT_TAGS:  asset_browser_edit_tags_rect(ab_area, &fx, &fy, &fw, &fh); inside = point_in_rect((float)x, (float)y, fx, fy, fw, fh); break;
                default: break;
            }
        }
        if (!inside) ab->focus = AB_FOCUS_NONE;
    }

    /* Chat input blur: same click-away-dismisses convention as the Asset
     * Browser block just above, single field so no switch needed. */
    if (button == 0 && pressed && ctx->chat && ctx->chat->focus == CHAT_FOCUS_INPUT) {
        Area *chat_area = g_ui.root ? find_area_by_type_r(g_ui.root, PANEL_CHAT) : NULL;
        int inside = 0;
        if (chat_area) {
            float fx, fy, fw, fh;
            chat_input_rect(chat_area, &fx, &fy, &fw, &fh);
            inside = point_in_rect((float)x, (float)y, fx, fy, fw, fh);
        }
        if (!inside) ctx->chat->focus = CHAT_FOCUS_NONE;
    }

    /* Border drag-to-resize starts here (checked before the top-chrome-
     * strip early return below, and before the recursive leaf hit-test,
     * since a border press is a distinct gesture that should win over
     * whatever's directly underneath it -- same "grabbing wins over
     * what's behind it" priority the gizmo handles already get over
     * scene-object picking). Only the press matters here; the drag itself
     * is continuous, driven every frame by ui_update_area_drag (called
     * from main.c), same split from click-edge to per-frame-continuation
     * the gizmo drag already uses. */
    if (button == 0 && pressed && g_ui.root) {
        Area *border = area_tree_find_border_hit(g_ui.root, x, y);
        if (border) {
            g_ui.resizing_area = border;
            return 1;
        }
    }

    if (y < (int)UI_TOP_CHROME_H) return 1;  /* branding bar + menu row claim the whole strip, nothing to route through it yet */
    if (g_ui.root && hit_test_area(g_ui.root, x, y, button, pressed, ctx)) return 1;
    return 0;
}

void ui_on_mouse_move(int x, int y) {
    /* Purely visual hover highlight for resize borders -- see
     * area_tree_find_border_hit/ui_render's border-highlight block. Skipped
     * entirely while a drag or any menu is already in progress, so hover
     * detection doesn't fight with (or redundantly recompute over) an
     * already-active interaction. */
    if (g_ui.resizing_area || g_ui.ctx_menu_open || g_ui.area_menu_open || !g_ui.root) {
        g_ui.hover_border = NULL;
        return;
    }
    g_ui.hover_border = area_tree_find_border_hit(g_ui.root, x, y);
}

/* Mouse wheel routing -- by HOVER (area_tree_find_leaf_at), not by click-
 * focus, deliberately: you shouldn't have to click into the Chat panel
 * just to scroll it, and the Scene panel's own zoom (below) explicitly
 * must NOT require clicking into the 3D view first, since that's exactly
 * what a free/fly camera user expects to just work by hovering. delta is
 * InputState::scroll_delta, drained by main.c the same frame this is
 * called -- see input.h's own comment for the sign convention (positive =
 * wheel scrolled up/toward the user).
 *
 * Each scrollable panel's own draw_panel_* function re-clamps its offset
 * to whatever's actually valid EVERY frame (list/log length can change
 * out from under a stale offset -- a delete, a new reply, a resize), so
 * this handler just accumulates the raw delta with the right sign for
 * that panel's own anchor convention (see ui_draw_scrollbar's own
 * comment: Chat/Console are bottom-anchored logs where positive delta
 * means "go back into history", Asset Browser/Python Panel are ordinary
 * top-anchored lists where positive delta means "move toward the top",
 * the opposite sign) rather than duplicating the clamp math here too. */
void ui_on_mouse_wheel(int x, int y, int delta, const UIRenderContext *ctx) {
    if (delta == 0 || !g_ui.root) return;
    Area *hover = area_tree_find_leaf_at(g_ui.root, x, y);
    if (!hover) return;

    switch (hover->panel_type) {
        case PANEL_CHAT:
            if (ctx->chat) ctx->chat->scroll_offset += delta;
            break;
        case PANEL_CONSOLE:
            if (ctx->console) ctx->console->scroll_offset += delta;
            break;
        case PANEL_ASSET_BROWSER:
            if (ctx->asset_browser) ctx->asset_browser->list_scroll_offset -= delta;
            break;
        case PANEL_PYTHON:
            g_ui.python_panel_scroll -= delta;
            break;
        case PANEL_SCENE:
            /* Camera zoom -- main.c owns the actual camera state (see
             * phi.md's Phase 1 status on why: no real camera-navigation
             * system exists yet beyond this), ui.c just routes the
             * hover-gated wheel event to it via this callback, same
             * shape draw_scene_content already established for handing
             * scene-content drawing back to main.c. */
            if (ctx->on_scene_zoom) ctx->on_scene_zoom(delta);
            break;
        default:
            break;
    }
}

void ui_open_scene_context_menu(int x, int y) {
    g_ui.ctx_menu_open = 1;
    g_ui.ctx_menu_x = (float)x;
    g_ui.ctx_menu_y = (float)y;
}

int ui_is_context_menu_open(void) { return g_ui.ctx_menu_open; }

CtxMenuAction ui_poll_context_menu_action(void) {
    CtxMenuAction a = g_ui.ctx_menu_pending_action;
    g_ui.ctx_menu_pending_action = CTX_ACTION_NONE;
    return a;
}

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
