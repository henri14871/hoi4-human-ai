@echo off
rem Builds native\build\version.dll (x64, release) with the installed Visual Studio.
setlocal
set "HAI_VS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem "call" keeps the parentheses in the path from breaking for /f.
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -property installationPath`) do set "HAI_VS=%%i"
if not defined HAI_VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%HAI_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
cd build

set CFLAGS=/nologo /c /O2 /MT /EHa /std:c++17 /W4 /wd4100 /wd4127 /DWIN32_LEAN_AND_MEAN /DNOMINMAX

ml64 /nologo /c /Fo asm.obj ..\src\asm.asm || exit /b 1

cl %CFLAGS% ..\src\proxy.cpp ..\src\scan.cpp ..\src\hai.cpp ..\src\overlay.cpp || exit /b 1

cl %CFLAGS% /w /I..\imgui ^
   ..\imgui\imgui.cpp ..\imgui\imgui_draw.cpp ..\imgui\imgui_tables.cpp ..\imgui\imgui_widgets.cpp ^
   ..\imgui\backends\imgui_impl_dx11.cpp ..\imgui\backends\imgui_impl_win32.cpp || exit /b 1

link /nologo /DLL /OUT:version.dll /DEF:..\src\version.def ^
   proxy.obj scan.obj hai.obj overlay.obj asm.obj ^
   imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj ^
   imgui_impl_dx11.obj imgui_impl_win32.obj ^
   kernel32.lib shell32.lib ole32.lib uuid.lib d3d11.lib dxgi.lib || exit /b 1
echo Built %CD%\version.dll
