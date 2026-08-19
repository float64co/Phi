#pragma once

/* Real GL texture loading + caching, backed by stb_image.h (vendored
 * alongside stb_image_write.h -- see that file's own comment; this is
 * the read/decode half, PNG/JPEG/BMP/TGA/GIF among others). Added
 * 2026-08-19 to give the PBR/skinned shaders (see renderer.c) something
 * real to sample -- until now this codebase's material system was flat
 * per-face/per-object color only, no texture support anywhere (see
 * HEFace::texture's own comment in halfedge.h for the load-bearing use
 * case this closes: real Sketchfab-style multi-material character
 * models, which carry almost all of their actual appearance in texture
 * maps, not in glTF's baseColorFactor). */

/* Loads path via stb_image, uploads it as a real 2D texture (RGBA8,
 * linear filtering, clamp-to-edge, real mipmaps), and caches the result
 * keyed by the exact path string -- a second call with the same path
 * returns the same GL texture name without re-decoding or re-uploading
 * (a real, common case: this codebase's own character assets reference
 * a handful of source images across many materials). Returns 0 (GL's
 * own "no texture" name -- see HEFace::texture's own comment on why this
 * doubles as a safe, harmless failure sentinel) on a missing file or an
 * stb_image decode failure; logs the reason either way rather than
 * failing silently. */
unsigned int texture_cache_load(const char *path);

/* Deletes every cached texture and clears the cache. Call once at
 * shutdown if reclaiming GPU memory matters; safe to never call for a
 * short-lived process (the GL context teardown reclaims it anyway, same
 * convention this codebase's other GL resources already follow -- see
 * e.g. gbuffer.c's own free_gl_resources). */
void texture_cache_shutdown(void);
