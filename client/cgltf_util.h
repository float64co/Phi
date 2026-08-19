#pragma once
#include <string.h>
#include <stdio.h>

/* Shared parse+load_buffers boilerplate -- halfedge_gltf.c, skinned_mesh.c,
 * and skinned_mesh_object.c each used to repeat this exact memset+parse+
 * load_buffers sequence (only the failure-log tag differed). Header-only/
 * `static inline` rather than a new .c file + Makefile entry: this has no
 * GL dependency at all (just cgltf.h, already a shared dependency of all
 * three callers), so it's safe to include from the GL-free-linked ones
 * (halfedge_gltf.c/skinned_mesh.c/skinned_mesh_object.c are all linked into
 * phi_h_test with zero GL -- see the Makefile's PHI_H_TEST_SRCS) without
 * risking a GL dependency creeping in, the same concern that keeps this
 * codebase's small math helpers (mat4_mul et al.) deliberately duplicated
 * per-file rather than centralized in a header that pulls in renderer.h.
 *
 * Deliberately does NOT #include "cgltf.h" itself: that header's own
 * single-header-library convention only guards its DECLARATIONS against
 * being included twice in one translation unit -- its CGLTF_IMPLEMENTATION
 * function-body section has no such guard, since it's designed to be
 * activated by exactly one #include in exactly one .c file. halfedge_gltf.c
 * does exactly that (its own top comment says so); a second #include "cgltf.h"
 * from THIS header, in the same translation unit, re-emits every cgltf_*
 * function body and fails to compile with "redefinition of ..." errors
 * (confirmed by hitting this directly). Every real caller already includes
 * "cgltf.h" before this header, so it's never actually missing -- this file
 * just doesn't re-assert a dependency its callers already declare.
 *
 * Returns a fully parsed AND buffer-loaded cgltf_data* on success (caller
 * owns it, frees via cgltf_free same as before), NULL on failure at either
 * step. If log_tag is non-NULL, prints the same two failure messages each
 * caller already printed ("[tag] failed to parse %s" / "[tag] failed to
 * load buffers for %s") -- pass NULL to stay silent on failure (skinned_
 * mesh_object.c's own existing behavior: it silently proceeds with
 * clip_count=0 rather than logging a second, redundant failure for a file
 * skinned_mesh_load_gltf already successfully validated once). */
static inline cgltf_data *cgltf_parse_and_load(const char *path, const char *log_tag) {
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;

    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
        if (log_tag) printf("[%s] failed to parse %s\n", log_tag, path);
        return NULL;
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        if (log_tag) printf("[%s] failed to load buffers for %s\n", log_tag, path);
        cgltf_free(data);
        return NULL;
    }
    return data;
}
