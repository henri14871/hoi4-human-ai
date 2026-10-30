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

ml64 /nologo /c /Fo asm.obj ..\src\asm.asm || exit /b 1
cl /nologo /c /O2 /MT /EHa /std:c++17 /W4 /wd4100 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   ..\src\proxy.cpp ..\src\scan.cpp ..\src\hai.cpp || exit /b 1
link /nologo /DLL /OUT:version.dll /DEF:..\src\version.def ^
   proxy.obj scan.obj hai.obj asm.obj kernel32.lib shell32.lib ole32.lib uuid.lib || exit /b 1
echo Built %CD%\version.dll
