/* Must be included WITHOUT gl_native.h's #define block active on these
 * names, so the extern definitions below declare the real (unprefixed)
 * function-pointer variables that gl_native.h's macros then alias glFoo to. */
#include <GL/gl.h>      /* GLenum/GLuint/GLsizei base types glext.h needs */
#include <GL/glext.h>
#include "phi_platform.h"

#define GL_NATIVE_PROC_LIST \
    X(PFNGLCREATESHADERPROC,             glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,             glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,            glCompileShader) \
    X(PFNGLGETSHADERIVPROC,              glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,         glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC,            glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,             glAttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC,       glBindAttribLocation) \
    X(PFNGLLINKPROGRAMPROC,              glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,             glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,        glGetProgramInfoLog) \
    X(PFNGLDELETESHADERPROC,             glDeleteShader) \
    X(PFNGLDELETEPROGRAMPROC,            glDeleteProgram) \
    X(PFNGLGENBUFFERSPROC,               glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,               glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,               glBufferData) \
    X(PFNGLGETUNIFORMLOCATIONPROC,       glGetUniformLocation) \
    X(PFNGLUSEPROGRAMPROC,               glUseProgram) \
    X(PFNGLUNIFORMMATRIX4FVPROC,         glUniformMatrix4fv) \
    X(PFNGLUNIFORM3FVPROC,               glUniform3fv) \
    X(PFNGLUNIFORM3FPROC,                glUniform3f) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIB1FPROC,           glVertexAttrib1f) \
    X(PFNGLGENVERTEXARRAYSPROC,          glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray)

#define X(type, name) type name##_native = 0;
GL_NATIVE_PROC_LIST
#undef X

void gl_native_load_procs(void) {
#define X(type, name) name##_native = (type)phi_gl_get_proc(#name);
    GL_NATIVE_PROC_LIST
#undef X
}
