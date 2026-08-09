#include "svg_icon.h"
#define NANOSVG_IMPLEMENTATION
#include "vendor/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "vendor/nanosvg/nanosvgrast.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif

SvgIcon svg_icon_load(const char *path, int size) {
    SvgIcon icon = {0, size};

    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) {
        printf("[svg_icon] failed to parse '%s'\n", path);
        return icon;
    }
    if (image->width <= 0.0f || image->height <= 0.0f) {
        printf("[svg_icon] '%s' has no width/height\n", path);
        nsvgDelete(image);
        return icon;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return icon;
    }

    float scale = (float)size / (image->width > image->height ? image->width : image->height);
    float tx = ((float)size - image->width  * scale) * 0.5f;
    float ty = ((float)size - image->height * scale) * 0.5f;

    unsigned char *pixels = (unsigned char *)calloc((size_t)size * (size_t)size * 4, 1);
    nsvgRasterize(rast, image, tx, ty, scale, pixels, size, size, size * 4);

    /* Recolor to a pure coverage mask (white RGB, SVG-derived alpha) --
     * see this file's header comment. */
    for (int i = 0; i < size * size; i++) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 255;
        pixels[i * 4 + 2] = 255;
    }

    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    glGenTextures(1, &icon.texture);
    glBindTexture(GL_TEXTURE_2D, icon.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(pixels);
    return icon;
}

void svg_icon_destroy(SvgIcon *icon) {
    if (icon->texture) glDeleteTextures(1, &icon->texture);
    icon->texture = 0;
}
