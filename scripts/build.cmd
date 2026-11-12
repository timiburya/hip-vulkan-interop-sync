@echo off
rem Configure + build into <repo>\build. Needs: HIP SDK (HIP_PATH), Vulkan SDK (VULKAN_SDK),
rem Visual Studio (STL + Windows SDK headers only; compilers are ROCm clang), CMake, Ninja.
rem The GPU arch is auto-detected; override with: scripts\build.cmd -DCMAKE_HIP_ARCHITECTURES=gfxXXXX
setlocal
set "ROOT=%~dp0.."
if defined VSCMD_VER goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo vswhere not found; run from a Developer prompt & exit /b 1
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT echo Visual Studio not found & exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release %* || exit /b 1
cmake --build "%ROOT%\build" || exit /b 1
echo BUILD OK
