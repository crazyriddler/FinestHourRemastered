@echo off
rem The lighting bake's tool (bake.exe beside it, with Embree's DLLs): tools\bake\build.cmd
rem Embree 4 from https://github.com/RenderKit/embree/releases (x64 windows), unpacked in ..\..\..\tools\embree
setlocal
set "HERE=%~dp0"
set "EMBREE=%HERE%..\..\..\tools\embree\embree-4.4.1.x64.windows"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%HERE%..\..\..\tools\llvm\bin;%PATH%"
clang-cl /nologo /O2 /EHsc /std:c++20 /I"%EMBREE%\include" "%HERE%bake.cpp" /Fe"%HERE%bake.exe" /Fo"%TEMP%\bake.obj" /link "%EMBREE%\lib\embree4.lib" cabinet.lib || exit /b 1
copy /y "%EMBREE%\bin\embree4.dll" "%HERE%" >nul
copy /y "%EMBREE%\bin\tbb12.dll" "%HERE%" >nul
copy /y "%EMBREE%\bin\tbbmalloc.dll" "%HERE%" >nul
