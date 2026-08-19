#pragma once
/* Minimal MSVC compatibility shim for <GL/wglext.h> -- this is a real
 * Khronos OpenGL-registry header (WGL_ARB_create_context etc.), not part
 * of the Windows SDK or MSVC's own headers, and not vendored anywhere
 * else in this repo (client/vendor has no GL/ tree at all). mingw-w64
 * doesn't ship it either, but the Makefile's win32 target never actually
 * needs it (see gap #2 in build.bat's own top comment for the general
 * shape of these two toolchains diverging).
 *
 * phi_platform_win32.c only needs a handful of symbols from the real
 * header (verified by reading it directly, not assumed) to create a
 * modern OpenGL 3.3 core-profile context via wglCreateContextAttribsARB
 * -- the WGL_ARB_create_context / WGL_ARB_create_context_profile
 * extensions' constants and one function-pointer typedef. Values below
 * are the real, stable, publicly-documented ones from those extension
 * specs, not placeholders.
 *
 * build.bat adds this directory's parent to the include path LAST (after
 * every real include dir), so if a real GL/wglext.h ever legitimately
 * existed somewhere on the include path first, it would still win over
 * this. */
#include <windows.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001

typedef HGLRC (WINAPI * PFNWGLCREATECONTEXTATTRIBSARBPROC) (HDC hDC, HGLRC hShareContext, const int *attribList);
