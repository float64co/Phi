#pragma once

/* SVG icon loading — rasterizes an .svg (via vendored nanosvg) once at
 * load time into a square GL_RGBA8 texture, recolored to a white-RGB/
 * SVG-alpha coverage mask (not the SVG's own fill color) so the UI's tint
 * color decides the final color per draw, same technique as the SDF font
 * atlas being a distance mask rather than pre-colored glyphs. Logic
 * adapted from a separate reference project's SvgIcon.cpp (read for the
 * technique, not copied — that version depends on glad.h, a GL loader
 * library phi.md's Hard Architectural Decisions explicitly rule out; this
 * one uses Phi's own gl_native.h/wasm GL setup, no new proc-fetch entries
 * needed since every GL call here is already used elsewhere in the
 * codebase). */

typedef struct {
    unsigned int texture;
    int          size;   /* square, texture is size x size */
} SvgIcon;

/* Rasterizes `path` at `size` x `size` pixels (aspect-preserving, letter-
 * boxed if the SVG's own width/height isn't square). Returns a
 * zero-initialized (texture=0) SvgIcon on failure (missing file, parse
 * error, zero width/height) — callers should check .texture != 0 before
 * drawing rather than assume success. */
SvgIcon svg_icon_load(const char *path, int size);
void    svg_icon_destroy(SvgIcon *icon);
