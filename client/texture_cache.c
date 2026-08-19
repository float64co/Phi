#include "texture_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#include "gl_native.h"
#endif

/* Exactly one translation unit defines STB_IMAGE_IMPLEMENTATION to get
 * stb_image's function bodies (not just declarations) -- this is that
 * one, same "one TU owns the real implementation" convention halfedge_
 * gltf.c already established for CGLTF_IMPLEMENTATION. Only PNG/JPEG
 * decoding is compiled in (this codebase's actual asset set -- see
 * halfedge_gltf.c/skinned_mesh.c's own texture loading), trimming the
 * rest of stb_image's format support (BMP/TGA/GIF/PSD/HDR/PIC/PNM) out
 * of the build entirely rather than silently carrying dead code for
 * formats nothing here ever produces or consumes. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static void gl_check(const char *where) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR)
        printf("[GL] error 0x%04x at %s\n", e, where);
}

typedef struct {
    char        *path;
    unsigned int texture;
} CacheEntry;

#define TEXTURE_CACHE_MAX 256
static CacheEntry s_cache[TEXTURE_CACHE_MAX];
static int        s_cache_count = 0;

unsigned int texture_cache_load(const char *path) {
    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].path, path) == 0) return s_cache[i].texture;
    }

    int w, h, channels;
    /* Force 4 channels (RGBA) regardless of the source file's own real
     * channel count -- a real, uniform upload format rather than
     * branching GL_RGB vs. GL_RGBA per file (some of this project's own
     * character textures are 3-channel JPEGs, others 4-channel PNGs with
     * real alpha; stb_image's own req_comp parameter handles the
     * conversion, not hand-rolled here). */
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        printf("[texture_cache] failed to load '%s': %s\n", path, stbi_failure_reason());
        return 0;
    }

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    gl_check("texture_cache_load");

    stbi_image_free(pixels);

    printf("[texture_cache] loaded '%s' (%dx%d, tex=%u)\n", path, w, h, tex);

    if (s_cache_count < TEXTURE_CACHE_MAX) {
        s_cache[s_cache_count].path = strdup(path);
        s_cache[s_cache_count].texture = tex;
        s_cache_count++;
    } else {
        printf("[texture_cache] cache full (TEXTURE_CACHE_MAX=%d) -- '%s' loaded but not cached, "
               "a repeat load will re-decode/re-upload\n", TEXTURE_CACHE_MAX, path);
    }
    return tex;
}

void texture_cache_shutdown(void) {
    for (int i = 0; i < s_cache_count; i++) {
        glDeleteTextures(1, &s_cache[i].texture);
        free(s_cache[i].path);
    }
    s_cache_count = 0;
}
