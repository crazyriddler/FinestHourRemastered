@echo off
rem Local build with the LLVM in ..\tools\llvm: build_local.cmd [release|debug|relwithdebinfo]
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%~dp0..\tools\llvm\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
if not exist "out\build\%PRESET%\build.ninja" cmake --preset %PRESET% || exit /b 1
if "%2"=="" (cmake --build "out\build\%PRESET%") else (cmake --build "out\build\%PRESET%" --target %2)
