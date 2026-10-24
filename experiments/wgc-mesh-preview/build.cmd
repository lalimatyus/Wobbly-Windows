@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE main.cpp /Fe:wgc-mesh-preview.exe /link d3d11.lib d3dcompiler.lib dxgi.lib windowsapp.lib user32.lib
exit /b %errorlevel%
