#pragma once
/* Minimal MSVC compatibility shim for <GL/glext.h> -- this is a real
 * Khronos OpenGL-registry header, not part of the Windows SDK or MSVC's
 * own headers (the SDK's GL/gl.h only covers OpenGL 1.1) and not
 * vendored anywhere else in this repo. mingw-w64 doesn't ship it either,
 * but the Makefile's win32 target reaches an actual glext.h some other
 * way not reproduced here (not needed -- see build.bat's own top
 * comment for this toolchain's separate MSVC-vs-mingw gaps).
 *
 * gl_native.h/gl_native.c only need the function-pointer typedefs for the
 * exact GL_NATIVE_PROC_LIST entries those two files define (verified by
 * reading them directly, not assumed) -- every core OpenGL 2.0/3.0
 * function this renderer fetches at runtime via phi_gl_get_proc, same
 * "hand-roll exactly what's needed" precedent as
 * client/msvc_compat/GL/wglext.h. Signatures below match the real
 * Khronos glext.h exactly (real, stable, publicly-documented ABI, not
 * placeholders) -- these are real stdcall (APIENTRY) function pointers
 * that get assigned the actual opengl32.dll-exported addresses at
 * runtime, so a mismatched signature here would silently corrupt the
 * stack on every call.
 *
 * build.bat adds this directory's parent to the include path LAST (after
 * every real include dir), so if a real GL/glext.h ever legitimately
 * existed somewhere on the include path first, it would still win over
 * this. */
#include <stddef.h>

typedef char GLchar;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;

/* GL 1.5+/2.0+/3.0+ enum constants the Windows SDK's GL/gl.h (OpenGL 1.1
 * only) doesn't define, collected by grepping every GL_* identifier
 * actually referenced across octree_render.c/renderer.c/texture_cache.c/
 * gl_native.c/.h/gbuffer.c/skinned_scene_objects.c/svg_icon.c/ui.c/
 * font.c/phi_platform_*.c/editor_main.c/player_main.c -- real, stable,
 * publicly-documented core-profile values (identical across every GL
 * loader/header in existence), not placeholders. #ifndef-guarded so
 * this is a no-op for any of these gl.h 1.1 *does* already define
 * (several of the grepped names, e.g. GL_RED, predate 1.5 and likely
 * already are) -- never overrides the real header. */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_TEXTURE3
#define GL_TEXTURE3 0x84C3
#endif
#ifndef GL_TEXTURE4
#define GL_TEXTURE4 0x84C4
#endif
#ifndef GL_TEXTURE5
#define GL_TEXTURE5 0x84C5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_COLOR_ATTACHMENT2
#define GL_COLOR_ATTACHMENT2 0x8CE2
#endif
#ifndef GL_COLOR_ATTACHMENT3
#define GL_COLOR_ATTACHMENT3 0x8CE3
#endif
#ifndef GL_COLOR_ATTACHMENT4
#define GL_COLOR_ATTACHMENT4 0x8CE4
#endif
#ifndef GL_COLOR_ATTACHMENT5
#define GL_COLOR_ATTACHMENT5 0x8CE5
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif
#ifndef GL_R11F_G11F_B10F
#define GL_R11F_G11F_B10F 0x8C3A
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif
#ifndef GL_RG16F
#define GL_RG16F 0x822F
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_UNSIGNED_INT_10F_11F_11F_REV
#define GL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#endif
#ifndef GL_UNSIGNED_INT_2_10_10_10_REV
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#endif

typedef void      (APIENTRY *PFNGLACTIVETEXTUREPROC)             (GLenum texture);
typedef void      (APIENTRY *PFNGLATTACHSHADERPROC)              (GLuint program, GLuint shader);
typedef void      (APIENTRY *PFNGLBINDATTRIBLOCATIONPROC)        (GLuint program, GLuint index, const GLchar *name);
typedef void      (APIENTRY *PFNGLBINDBUFFERPROC)                (GLenum target, GLuint buffer);
typedef void      (APIENTRY *PFNGLBINDFRAMEBUFFERPROC)           (GLenum target, GLuint framebuffer);
typedef void      (APIENTRY *PFNGLBINDVERTEXARRAYPROC)           (GLuint array);
typedef void      (APIENTRY *PFNGLBUFFERDATAPROC)                (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef GLenum    (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC)    (GLenum target);
typedef void      (APIENTRY *PFNGLCLEARBUFFERUIVPROC)            (GLenum buffer, GLint drawbuffer, const GLuint *value);
typedef void      (APIENTRY *PFNGLCOMPILESHADERPROC)             (GLuint shader);
typedef GLuint    (APIENTRY *PFNGLCREATEPROGRAMPROC)             (void);
typedef GLuint    (APIENTRY *PFNGLCREATESHADERPROC)              (GLenum type);
typedef void      (APIENTRY *PFNGLDELETEBUFFERSPROC)             (GLsizei n, const GLuint *buffers);
typedef void      (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC)        (GLsizei n, const GLuint *framebuffers);
typedef void      (APIENTRY *PFNGLDELETEPROGRAMPROC)             (GLuint program);
typedef void      (APIENTRY *PFNGLDELETESHADERPROC)              (GLuint shader);
typedef void      (APIENTRY *PFNGLDISABLEVERTEXATTRIBARRAYPROC)  (GLuint index);
typedef void      (APIENTRY *PFNGLDRAWBUFFERSPROC)               (GLsizei n, const GLenum *bufs);
typedef void      (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)   (GLuint index);
typedef void      (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC)      (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef void      (APIENTRY *PFNGLGENBUFFERSPROC)                (GLsizei n, GLuint *buffers);
typedef void      (APIENTRY *PFNGLGENFRAMEBUFFERSPROC)           (GLsizei n, GLuint *framebuffers);
typedef void      (APIENTRY *PFNGLGENVERTEXARRAYSPROC)           (GLsizei n, GLuint *arrays);
typedef void      (APIENTRY *PFNGLGENERATEMIPMAPPROC)            (GLenum target);
typedef void      (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)         (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void      (APIENTRY *PFNGLGETPROGRAMIVPROC)              (GLuint program, GLenum pname, GLint *params);
typedef void      (APIENTRY *PFNGLGETSHADERINFOLOGPROC)          (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void      (APIENTRY *PFNGLGETSHADERIVPROC)               (GLuint shader, GLenum pname, GLint *params);
typedef GLint     (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)        (GLuint program, const GLchar *name);
typedef void      (APIENTRY *PFNGLLINKPROGRAMPROC)               (GLuint program);
typedef void      (APIENTRY *PFNGLSHADERSOURCEPROC)              (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
typedef void      (APIENTRY *PFNGLUNIFORM1FPROC)                 (GLint location, GLfloat v0);
typedef void      (APIENTRY *PFNGLUNIFORM1IPROC)                 (GLint location, GLint v0);
typedef void      (APIENTRY *PFNGLUNIFORM1UIPROC)                (GLint location, GLuint v0);
typedef void      (APIENTRY *PFNGLUNIFORM2FPROC)                 (GLint location, GLfloat v0, GLfloat v1);
typedef void      (APIENTRY *PFNGLUNIFORM3FPROC)                 (GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
typedef void      (APIENTRY *PFNGLUNIFORM3FVPROC)                (GLint location, GLsizei count, const GLfloat *value);
typedef void      (APIENTRY *PFNGLUNIFORM4FPROC)                 (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void      (APIENTRY *PFNGLUNIFORMMATRIX4FVPROC)          (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void      (APIENTRY *PFNGLUSEPROGRAMPROC)                (GLuint program);
typedef void      (APIENTRY *PFNGLVERTEXATTRIB1FPROC)            (GLuint index, GLfloat x);
typedef void      (APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)       (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
