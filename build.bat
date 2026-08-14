@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  Phi -- Windows build via real MSVC (cl.exe), no CMake.
REM
REM  Mirrors the Makefile's `win32` target as closely as possible
REM  (see Makefile's WIN32_SRCS/WIN32_CFLAGS/WIN32_LDFLAGS), translated
REM  to MSVC's own flag syntax: same source list, same include paths,
REM  same Win32 import libraries. Run from the repo root -- either from
REM  a "Developer Command Prompt for VS" (cl.exe already on PATH) or a
REM  plain cmd.exe (this script will locate and invoke vcvarsall.bat
REM  itself via vswhere.exe if cl.exe isn't already available).
REM
REM  Output: build\phi_win32.exe  (same path the Makefile's own win32
REM  target produces, via a completely different toolchain -- WSL +
REM  mingw-w64 gcc.exe reached over WSL interop there, real MSVC here).
REM
REM  TWO REAL PORTABILITY GAPS BETWEEN MINGW-W64 AND PLAIN MSVC, found by
REM  reading the vendored source directly (not guessed), since this
REM  script was written in a Linux sandbox with no MSVC available to
REM  actually test it against:
REM
REM  1. FIXED HERE: several vendored MicroPython files unconditionally
REM     `#include <unistd.h>` (a POSIX header mingw-w64 ships a
REM     compatibility version of, but plain MSVC's CRT does not ship at
REM     all). Confirmed by direct inspection that the only thing any of
REM     them actually need from it is the ssize_t typedef -- the real
REM     POSIX read()/write()/close() calls those same files also
REM     reference are all inside `#if MICROPY_READER_POSIX` / `#if
REM     MICROPY_PERSISTENT_CODE_SAVE_FILE` blocks, both 0 in this
REM     project's own mpconfigport.h, so those bodies never actually
REM     compile on any toolchain. Fixed with a 3-line compatibility
REM     header (client\msvc_compat\unistd.h) added to the include path
REM     LAST, so a real unistd.h would still win if one ever existed.
REM
REM  2. NOT FIXED, UNVERIFIED, FLAGGED HONESTLY: this codebase uses C99
REM     compound-literal syntax extensively (`(Vec3f){1,2,3}`-shaped
REM     expressions, in ~38 client/ files) and, in one spot
REM     (client\main.c, PhiPlatformConfig), a C99 designated initializer
REM     (`{ .title = "Phi", ... }`). GCC/mingw-w64/clang all accept both
REM     as standard C99 (or as GNU extensions in C++ mode); real MSVC's C
REM     front end has a long, well-documented history of NOT supporting
REM     compound literals in particular, even under /std:c11 or
REM     /std:c17 (designated initializers fare somewhat better but are
REM     not guaranteed either). This script cannot verify whether that's
REM     still true of whatever MSVC version you have installed -- there
REM     is no MSVC in the sandbox this was written in. If compilation
REM     fails with syntax errors pointing at lines shaped like
REM     `(SomeType){...}`, the most likely real fix is switching CL below
REM     to clang-cl.exe instead (accepts the exact same command-line
REM     flags cl.exe does; installable via the Visual Studio Installer's
REM     optional "C++ Clang tools for Windows"
REM     component, or standalone LLVM) -- clang-cl supports compound
REM     literals as the real GNU extension they are. Not pre-applied
REM     here since it changes which compiler actually runs; flagged so
REM     it's a five-second fix instead of a confusing dead end.
REM ============================================================

set CL=cl.exe

where %CL% >nul 2>nul
if errorlevel 1 (
    echo [build.bat] %CL% not found on PATH -- looking for a Visual Studio installation via vswhere...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [build.bat] vswhere.exe not found. Either run this from a "Developer Command Prompt for VS", or install Visual Studio / Build Tools with the "Desktop development with C++" workload.
        exit /b 1
    )
    set "VSINSTALL="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
    if not defined VSINSTALL (
        echo [build.bat] No Visual Studio installation with the C++ ^(VC.Tools.x86.x64^) workload was found.
        exit /b 1
    )
    call "!VSINSTALL!\VC\Auxiliary\Build\vcvarsall.bat" x64
    if errorlevel 1 exit /b 1
    echo [build.bat] found and set up: !VSINSTALL!
)

set SRCDIR=client
set BUILDDIR=build
set OUT=%BUILDDIR%\phi_win32.exe
set RSPFILE=%BUILDDIR%\build.rsp

if not exist %BUILDDIR% mkdir %BUILDDIR%
if exist %RSPFILE% del %RSPFILE%

REM ---- Compiler flags + includes (matches Makefile's WIN32_CFLAGS) ----
REM /EHsc: standard C++ exception model, needed for the vendored Bullet
REM C++ sources and client\phi_physics.cpp. msvc_compat is last on
REM purpose (see gap #1 above).
echo /nologo>> %RSPFILE%
echo /O2>> %RSPFILE%
echo /W3>> %RSPFILE%
echo /EHsc>> %RSPFILE%
echo /I%SRCDIR%>> %RSPFILE%
echo /I%SRCDIR%\micropython_embed>> %RSPFILE%
echo /I%SRCDIR%\micropython_embed\port>> %RSPFILE%
echo /I%SRCDIR%\vendor\bullet3\src>> %RSPFILE%
echo /I%SRCDIR%\msvc_compat>> %RSPFILE%

REM ---- Client sources (matches Makefile's COMMON_SRCS, in the same
REM order, plus the win32-specific platform files) -- hand-listed since
REM this is a short, stable list the Makefile itself also hand-lists;
REM Bullet/MicroPython below are NOT hand-listed since those are 100s of
REM vendored files best discovered the same way the Makefile's own
REM wildcard does it, not hand-copied into a second list that could
REM silently drift out of sync with what's actually vendored. ----
for %%f in (
    main.c octree_render.c renderer.c net.c input.c console.c
    asset_browser.c chat.c halfedge.c halfedge_gltf.c meshobject.c
    mesh_edit.c fracture.c armature.c animation.c skinned_mesh.c
    gizmo.c transform_op.c light.c scene_target.c fracture_body.c font.c svg_icon.c ui.c area_tree.c phi_prop.c
    phi_prop_registry.c mp_port.c phi_physics.cpp
    phi_platform_win32.c gl_native.c gbuffer.c ws_client_win32.c
) do echo "%SRCDIR%\%%f">> %RSPFILE%

REM ---- Bullet (LinearMath+BulletCollision+BulletDynamics only -- see
REM phi.md's Phase 2 status for why only those three subtrees are
REM vendored) -- same two-directory-level shape as the Makefile's own
REM $(wildcard $(BULLET_DIR)/*/*.cpp) $(wildcard $(BULLET_DIR)/*/*/*.cpp),
REM just walked recursively here since nothing is vendored deeper than
REM that (verified directly, not assumed) so a full recursive walk finds
REM exactly the same file set. ----
for /r %SRCDIR%\vendor\bullet3\src %%f in (*.cpp) do echo "%%f">> %RSPFILE%

REM ---- MicroPython embed tree -- same reasoning as Bullet above. ----
for /r %SRCDIR%\micropython_embed %%f in (*.c) do echo "%%f">> %RSPFILE%

REM ---- Output + link (matches Makefile's WIN32_LDFLAGS, minus
REM -lstdc++: MSVC links its own C++ runtime automatically). ----
echo /Fe:%OUT%>> %RSPFILE%
echo /link>> %RSPFILE%
echo opengl32.lib>> %RSPFILE%
echo gdi32.lib>> %RSPFILE%
echo user32.lib>> %RSPFILE%
echo kernel32.lib>> %RSPFILE%
echo ws2_32.lib>> %RSPFILE%
echo bcrypt.lib>> %RSPFILE%

echo [build.bat] compiling (pulls in the full vendored Bullet + MicroPython trees -- expect this to take a while, same reason the Makefile's own win32 target is the slowest build in this project)...
%CL% @%RSPFILE%
if errorlevel 1 (
    echo.
    echo [build.bat] build FAILED -- see the two known portability gaps documented at the top of this script before debugging further.
    exit /b 1
)
echo [build.bat] build complete -^> %OUT%
