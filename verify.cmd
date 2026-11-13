@echo off
setlocal
cd /d "%~dp0"
if not exist build\libwebp-1.6.0-windows-x64\lib\libwebp.lib (
    call build.cmd
    if errorlevel 1 exit /b 1
)
for /f "usebackq delims=" %%v in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%v"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
python tests\make_fixtures.py
if errorlevel 1 exit /b 1
set "WEBP=%CD%\build\libwebp-1.6.0-windows-x64"
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%WEBP%\include" /Fo:build\ /Fe:build\image_checks.exe tests\image_checks.cpp src\image.cpp /link /LIBPATH:"%WEBP%\lib" libwebp.lib libwebpdemux.lib windowscodecs.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib
if errorlevel 1 exit /b 1
build\image_checks.exe
exit /b %errorlevel%
