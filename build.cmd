@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
set "WEBP=%CD%\build\libwebp-1.6.0-windows-x64"
if not exist "%WEBP%\lib\libwebp.lib" (
    powershell -NoProfile -File "%CD%\fetch-webp.ps1"
    if errorlevel 1 exit /b 1
)
if not defined VCToolsInstallDir (
    for /f "usebackq delims=" %%v in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%v"
)
if not defined VCToolsInstallDir (
    if not defined VSROOT (
        echo Visual Studio C++ x64 build tools are required.
        exit /b 1
    )
    call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 exit /b 1
)
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%WEBP%\include" /Fo:build\ src\main.cpp src\image.cpp build\app.res /Fe:PicCompare.exe /link /MANIFEST:NO /SUBSYSTEM:WINDOWS /LIBPATH:"%WEBP%\lib" libwebp.lib libwebpdemux.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib
exit /b %errorlevel%
