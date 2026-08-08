#pragma once
/* Native (desktop GL) proc-address loading.
 *
 * <GL/gl.h> only declares up to OpenGL 1.2 on Linux — everything the
 * renderer needs beyond that (shaders, VBOs, vertex attribs, VAOs) has to
 * be fetched at runtime via phi_gl_get_proc (glXGetProcAddressARB), the
 * same hand-rolled approach phi.md specifies instead of pulling in a
 * loader library like GLEW/GLAD.
 *
 * Included only by the native (#else) branch of renderer.c and
 * octree_render.c. The #define block below makes the plain glFoo(...)
 * spelling those files already use resolve to the fetched pointer, so
 * their call sites need zero changes. */

#include <GL/gl.h>      /* GLenum/GLuint/GLsizei base types glext.h needs */
#include <GL/glext.h>

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
    X(PFNGLUNIFORM4FPROC,                glUniform4f) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIB1FPROC,           glVertexAttrib1f) \
    X(PFNGLGENVERTEXARRAYSPROC,          glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray) \
    X(PFNGLGENFRAMEBUFFERSPROC,          glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,          glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,     glFramebufferTexture2D) \
    X(PFNGLDRAWBUFFERSPROC,              glDrawBuffers) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,   glCheckFramebufferStatus) \
    X(PFNGLDELETEFRAMEBUFFERSPROC,       glDeleteFramebuffers) \
    X(PFNGLACTIVETEXTUREPROC,            glActiveTexture) \
    X(PFNGLUNIFORM1FPROC,                glUniform1f) \
    X(PFNGLUNIFORM1IPROC,                glUniform1i) \
    X(PFNGLUNIFORM1UIPROC,               glUniform1ui) \
    X(PFNGLUNIFORM2FPROC,                glUniform2f) \
    X(PFNGLCLEARBUFFERUIVPROC,           glClearBufferuiv)

#define X(type, name) extern type name##_native;
GL_NATIVE_PROC_LIST
#undef X

#define glCreateShader              glCreateShader_native
#define glShaderSource              glShaderSource_native
#define glCompileShader             glCompileShader_native
#define glGetShaderiv               glGetShaderiv_native
#define glGetShaderInfoLog          glGetShaderInfoLog_native
#define glCreateProgram             glCreateProgram_native
#define glAttachShader              glAttachShader_native
#define glBindAttribLocation        glBindAttribLocation_native
#define glLinkProgram                glLinkProgram_native
#define glGetProgramiv               glGetProgramiv_native
#define glGetProgramInfoLog          glGetProgramInfoLog_native
#define glDeleteShader                glDeleteShader_native
#define glDeleteProgram               glDeleteProgram_native
#define glGenBuffers                  glGenBuffers_native
#define glBindBuffer                  glBindBuffer_native
#define glBufferData                  glBufferData_native
#define glGetUniformLocation          glGetUniformLocation_native
#define glUseProgram                  glUseProgram_native
#define glUniformMatrix4fv            glUniformMatrix4fv_native
#define glUniform3fv                  glUniform3fv_native
#define glUniform3f                   glUniform3f_native
#define glUniform4f                   glUniform4f_native
#define glEnableVertexAttribArray     glEnableVertexAttribArray_native
#define glVertexAttribPointer         glVertexAttribPointer_native
#define glDisableVertexAttribArray    glDisableVertexAttribArray_native
#define glVertexAttrib1f              glVertexAttrib1f_native
#define glGenVertexArrays             glGenVertexArrays_native
#define glBindVertexArray             glBindVertexArray_native
#define glGenFramebuffers             glGenFramebuffers_native
#define glBindFramebuffer             glBindFramebuffer_native
#define glFramebufferTexture2D        glFramebufferTexture2D_native
#define glDrawBuffers                 glDrawBuffers_native
#define glCheckFramebufferStatus      glCheckFramebufferStatus_native
#define glDeleteFramebuffers          glDeleteFramebuffers_native
#define glActiveTexture               glActiveTexture_native
#define glUniform1f                   glUniform1f_native
#define glUniform1i                   glUniform1i_native
#define glUniform1ui                  glUniform1ui_native
#define glUniform2f                   glUniform2f_native
#define glClearBufferuiv              glClearBufferuiv_native

/* Fetches every pointer above via phi_gl_get_proc. Must run once, after the
 * GL context is current and before any of the calls above — renderer_create()
 * does this first thing on the native path. */
void gl_native_load_procs(void);
