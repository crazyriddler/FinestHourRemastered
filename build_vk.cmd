@echo off
rem The Vulkan experiment's build (branch vulkan): its own tree, out\build\vk, on the generated sources the main
rem build keeps (XBE_CODEGEN off); build_vk.cmd [target]
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%~dp0..\tools\llvm\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
if not exist "out\build\vk\build.ninja" cmake --preset release -B out\build\vk -DXBE_CODEGEN=OFF || exit /b 1
if "%1"=="" (cmake --build "out\build\vk" --target CODFH) else (cmake --build "out\build\vk" --target %1)
