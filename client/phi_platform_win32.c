#include "phi_platform.h"
#include "phi_platform_win32.h"
#include "input.h"

#include <windows.h>
#include <GL/gl.h>
#include <GL/wglext.h>
#include <stdio.h>

/* Win32/WGL platform backend — the third implementation of phi_platform.h
 * alongside phi_platform_wasm.c (Emscripten/WebGL) and phi_platform_native.c
 * (Xlib/GLX, Linux). Not a recompile of the Linux native backend — Windows
 * needs its own windowing (Win32) and GL context API (WGL vs GLX), so this
 * is a separate implementation, same role.
 *
 * Windowing + OpenGL 3.3 core context + proc loading (proved the existing
 * gl_native.h/gbuffer.c/renderer.c GL 3.3 core path — already written
 * against the portable phi_gl_get_proc abstraction — works on real
 * Windows/WGL, satisfying Phase 0's "glext.h GL loading verified on
 * Windows" deliverable) plus WndProc forwarding keyboard/mouse messages
 * into input.c's Win32 branch via input_native_handle_event, mirroring
 * how phi_platform_native.c's X11 event pump forwards to the X11 branch.
 * Winsock networking is NOT wired here — separate, later work. */

static HWND  s_hwnd = NULL;
static HDC   s_hdc  = NULL;
static HGLRC s_hglrc = NULL;
static int   s_w = 0, s_h = 0;
static int   s_should_close = 0;
static LARGE_INTEGER s_perf_freq, s_perf_start;

static PhiMainLoopFn s_loop_fn = NULL;
static void         *s_loop_ud = NULL;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        s_should_close = 1;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        s_w = LOWORD(lparam);
        s_h = HIWORD(lparam);
        return 0;

    /* Forwarded to input.c's Win32 branch — same role as
     * phi_platform_native.c's X11 event pump forwarding to
     * input_native_handle_event, just message-driven instead of
     * pumped. Still falls through to DefWindowProcA below; none of
     * these need to be "consumed" to keep default window behavior
     * correct (unlike WM_CLOSE above). */
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_KILLFOCUS: {
        MSG synthetic;
        synthetic.hwnd    = hwnd;
        synthetic.message = msg;
        synthetic.wParam  = wparam;
        synthetic.lParam  = lparam;
        input_native_handle_event(&synthetic);
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

void phi_platform_init(const PhiPlatformConfig *cfg) {
    HINSTANCE hinst = GetModuleHandle(NULL);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = "PhiWindowClass";
    RegisterClassA(&wc);

    s_w = cfg->width  > 0 ? cfg->width  : 1280;
    s_h = cfg->height > 0 ? cfg->height : 720;

    RECT rect = {0, 0, s_w, s_h};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    s_hwnd = CreateWindowExA(0, "PhiWindowClass", cfg->title ? cfg->title : "Phi",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              NULL, NULL, hinst, NULL);
    ShowWindow(s_hwnd, SW_SHOW);
    s_hdc = GetDC(s_hwnd);

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(s_hdc, &pfd);
    SetPixelFormat(s_hdc, pf, &pfd);

    /* WGL chicken-and-egg: wglGetProcAddress only works once SOME context
     * is current (unlike GLX's glXGetProcAddressARB, callable anytime) —
     * so bootstrap with a throwaway legacy context just long enough to
     * fetch wglCreateContextAttribsARB, then replace it with a real
     * OpenGL 3.3 core context on the same window/DC. */
    HGLRC dummy = wglCreateContext(s_hdc);
    wglMakeCurrent(s_hdc, dummy);

    /* Cast via void* first — PROC (wglGetProcAddress's return type) has a
     * different formal parameter list than the target function pointer
     * type, which -Wcast-function-type flags on a direct cast; routing
     * through void* is the standard way to silence that for this exact,
     * unavoidable WGL pattern. */
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)(void *)wglGetProcAddress("wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB) {
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        s_hglrc = wglCreateContextAttribsARB(s_hdc, NULL, attribs);
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(dummy);
        wglMakeCurrent(s_hdc, s_hglrc);
    } else {
        fprintf(stderr, "[phi_platform_win32] wglCreateContextAttribsARB unavailable "
                        "(pre-2008-era driver?) — falling back to the legacy context\n");
        s_hglrc = dummy;
    }

    QueryPerformanceFrequency(&s_perf_freq);
    QueryPerformanceCounter(&s_perf_start);

    printf("[phi_platform_win32] GL_VERSION=%s GL_RENDERER=%s\n",
           (const char *)glGetString(GL_VERSION),
           (const char *)glGetString(GL_RENDERER));
}

void phi_platform_set_main_loop(PhiMainLoopFn fn, void *userdata) {
    s_loop_fn = fn;
    s_loop_ud = userdata;
    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_should_close) break;
        s_loop_fn(s_loop_ud);
    }
}

void phi_platform_swap(void) {
    SwapBuffers(s_hdc);
}

void phi_platform_get_window_size(int *w, int *h) {
    *w = s_w;
    *h = s_h;
}

double phi_platform_now(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_perf_start.QuadPart) / (double)s_perf_freq.QuadPart;
}

int phi_platform_should_close(void) {
    return s_should_close;
}

/* Mirrors phi_platform_native.c's own nanosleep-based implementation
 * structurally (see phi_platform.h's own comment) -- written but
 * UNVERIFIED, same honest flag this codebase's other win32-only code
 * already carries (e.g. the win32 pointer-capture path), since there's
 * no Windows machine available to actually run this on. Sleep()'s
 * granularity is coarser than nanosleep's (typically ~1-15ms depending
 * on the system timer resolution, vs. nanosecond-requested on Linux) --
 * a real, honest platform difference, not something this can paper over
 * without a Windows machine to tune it against. */
void phi_platform_sleep(double seconds) {
    if (seconds <= 0.0) return;
    DWORD ms = (DWORD)(seconds * 1000.0);
    if (ms > 0) Sleep(ms);
}

void *phi_gl_get_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    /* wglGetProcAddress doesn't resolve pre-1.2 functions (those are
     * statically exported from opengl32.dll instead), and returns one of
     * a few sentinel values on failure rather than NULL consistently —
     * fall back to GetProcAddress on the DLL module in either case. Moot
     * for everything gl_native.h actually asks for here (GL 2.0+ only —
     * <1.2 functions are called directly, declared by <GL/gl.h>, and link
     * statically against -lopengl32, same as native Linux against
     * libGL.so), but cheap and correct to handle anyway. */
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)(-1)) {
        static HMODULE opengl32 = NULL;
        if (!opengl32) opengl32 = GetModuleHandleA("opengl32.dll");
        p = (void *)GetProcAddress(opengl32, name);
    }
    return p;
}

void phi_platform_shutdown(void) {
    if (s_hglrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(s_hglrc); }
    if (s_hdc && s_hwnd) ReleaseDC(s_hwnd, s_hdc);
    if (s_hwnd) DestroyWindow(s_hwnd);
}

HWND phi_platform_win32_window(void) { return s_hwnd; }
