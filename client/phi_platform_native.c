#include "phi_platform.h"
#include "input.h"

#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

/* GLX_ARB_create_context — fetched via glXGetProcAddressARB rather than
 * relying on GLX_GLXEXT_PROTOTYPES static linking (see phi_gl_get_proc's
 * comment in phi_platform.h: no GL loader library, hand-rolled fetching). */
typedef GLXContext (*PFNGLXCREATECONTEXTATTRIBSARBPROC_LOCAL)(
    Display *dpy, GLXFBConfig config, GLXContext share_context,
    Bool direct, const int *attrib_list);

static Display     *s_dpy = NULL;
static Window        s_win;
static GLXContext    s_ctx;
static Atom          s_wm_delete;
static int           s_w = 0, s_h = 0;
static int           s_should_close = 0;

static PhiMainLoopFn s_loop_fn = NULL;
static void         *s_loop_ud = NULL;

void phi_platform_init(const PhiPlatformConfig *cfg) {
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy) {
        fprintf(stderr, "[phi_platform_native] XOpenDisplay failed "
                        "(no DISPLAY? this needs an X server — WSLg provides one)\n");
        exit(1);
    }
    int screen = DefaultScreen(s_dpy);

    int fb_attribs[] = {
        GLX_X_RENDERABLE,  True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE,      8,
        GLX_GREEN_SIZE,    8,
        GLX_BLUE_SIZE,     8,
        GLX_ALPHA_SIZE,    8,
        GLX_DEPTH_SIZE,    24,
        GLX_STENCIL_SIZE,  8,
        GLX_DOUBLEBUFFER,  True,
        None
    };
    int fbcount = 0;
    GLXFBConfig *fbc = glXChooseFBConfig(s_dpy, screen, fb_attribs, &fbcount);
    if (!fbc || fbcount == 0) {
        fprintf(stderr, "[phi_platform_native] glXChooseFBConfig found no matching framebuffer config\n");
        exit(1);
    }
    GLXFBConfig chosen = fbc[0];
    XFree(fbc);

    XVisualInfo *vi = glXGetVisualFromFBConfig(s_dpy, chosen);
    Window root = RootWindow(s_dpy, screen);

    XSetWindowAttributes swa;
    swa.colormap   = XCreateColormap(s_dpy, root, vi->visual, AllocNone);
    swa.event_mask = KeyPressMask | KeyReleaseMask | PointerMotionMask |
                     ButtonPressMask | ButtonReleaseMask | StructureNotifyMask |
                     FocusChangeMask;

    s_w = cfg->width  > 0 ? cfg->width  : 1280;
    s_h = cfg->height > 0 ? cfg->height : 720;

    s_win = XCreateWindow(s_dpy, root, 0, 0, (unsigned)s_w, (unsigned)s_h, 0,
                           vi->depth, InputOutput, vi->visual,
                           CWColormap | CWEventMask, &swa);
    XStoreName(s_dpy, s_win, cfg->title ? cfg->title : "Phi");

    s_wm_delete = XInternAtom(s_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(s_dpy, s_win, &s_wm_delete, 1);

    XMapWindow(s_dpy, s_win);

    PFNGLXCREATECONTEXTATTRIBSARBPROC_LOCAL CreateContextAttribsARB =
        (PFNGLXCREATECONTEXTATTRIBSARBPROC_LOCAL)
        glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    if (!CreateContextAttribsARB) {
        fprintf(stderr, "[phi_platform_native] glXCreateContextAttribsARB unavailable "
                        "(GLX_ARB_create_context not supported)\n");
        exit(1);
    }

    int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };
    s_ctx = CreateContextAttribsARB(s_dpy, chosen, NULL, True, ctx_attribs);
    XFree(vi);
    if (!s_ctx) {
        fprintf(stderr, "[phi_platform_native] Failed to create OpenGL 3.3 core context\n");
        exit(1);
    }
    XSync(s_dpy, False);
    glXMakeCurrent(s_dpy, s_win, s_ctx);

    printf("[phi_platform_native] GL_VERSION=%s GL_RENDERER=%s\n",
           (const char *)glGetString(GL_VERSION),
           (const char *)glGetString(GL_RENDERER));
}

static void pump_events(void) {
    while (XPending(s_dpy) > 0) {
        XEvent ev;
        XNextEvent(s_dpy, &ev);
        switch (ev.type) {
        case ConfigureNotify:
            s_w = ev.xconfigure.width;
            s_h = ev.xconfigure.height;
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == s_wm_delete)
                s_should_close = 1;
            break;
        default:
            /* Key/mouse events: dispatched to input.c once native input
             * handling lands (task tracked separately) — no-op stub today. */
            input_native_handle_event(&ev);
            break;
        }
    }
}

void phi_platform_set_main_loop(PhiMainLoopFn fn, void *userdata) {
    s_loop_fn = fn;
    s_loop_ud = userdata;
    for (;;) {
        pump_events();
        if (s_should_close) break;
        s_loop_fn(s_loop_ud);
    }
}

void phi_platform_swap(void) {
    glXSwapBuffers(s_dpy, s_win);
}

void phi_platform_get_window_size(int *w, int *h) {
    *w = s_w;
    *h = s_h;
}

double phi_platform_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int phi_platform_should_close(void) {
    return s_should_close;
}

void *phi_gl_get_proc(const char *name) {
    return (void *)glXGetProcAddressARB((const GLubyte *)name);
}

void phi_platform_shutdown(void) {
    if (!s_dpy) return;
    glXMakeCurrent(s_dpy, None, NULL);
    glXDestroyContext(s_dpy, s_ctx);
    XDestroyWindow(s_dpy, s_win);
    XCloseDisplay(s_dpy);
    s_dpy = NULL;
}
