@echo off
setlocal
set "GAME_TITLE=Finest Hour Remastered"
set "GAME_XBE=default.xbe"
set "GAME_EXE=CODFH"
title %GAME_TITLE% - Build
cd /d "%~dp0"
echo.
echo   %GAME_TITLE% - build
echo   ================================================
echo.

set "PF86=%ProgramFiles(x86)%"
set "VSINST=%PF86%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINST%\vswhere.exe"
set "OUT=%CD%\out\build\release"

if not exist assets\%GAME_XBE% goto :nogame

rem toolchain
echo [1/3] Toolchain
rem (call: cmd would take the quotes off a path with brackets in it, "Program Files (x86)")
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo       Visual Studio with the C++ workload was not found.
    echo       Install Visual Studio Community and tick "Desktop development with C++".
    goto :fail
)
call :setuptools
rem CI is set on GitHub Actions: no prompts there.
if not defined TOOLS_OK if defined CI goto :byhand
if not defined TOOLS_OK (
    choice /c YN /n /m "      Let Visual Studio install the missing components? [Y/N] "
    if errorlevel 2 goto :byhand
    echo       Close Visual Studio if it is open, it will be shut down anyway.
    echo       Installing. Accept the admin prompt, then wait for the installer.
    powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%VSINST%\setup.exe' -ArgumentList 'modify','--installPath','%VSDIR%','--add','Microsoft.VisualStudio.Component.VC.Llvm.Clang','--add','Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset','--add','Microsoft.VisualStudio.Component.VC.CMake.Project','--passive','--norestart','--force'"
    echo       Press a key once the Visual Studio Installer window has closed.
    pause >nul
    call :setuptools
    if not defined TOOLS_OK goto :byhand
)
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v
rem the release is built with LLVM 21.1.0 (MSVC 14.44, Windows SDK 10.0.26100): another version builds a game
rem that works, but not byte for byte the release's
clang++ --version | findstr /c:"clang version 21.1.0" >nul || echo       note: the release is built with LLVM 21.1.0 (set LLVM_DIR to it for the same exe)

rem symbols
echo [2/3] XDK symbols
cmake --preset release >nul || goto :fail
findstr /b /l /c:"[symbols]" config\game.toml >nul 2>nul && echo       config\game.toml already written && goto :game
cmake --build out\build\release --target xbe_symbols >nul || goto :fail
rem the configure step wrote where the SDK is
if not exist out\build\release\sdk_dir.txt echo       out\build\release\sdk_dir.txt was not written, update the SDK. && goto :fail
set /p SDK=<out\build\release\sdk_dir.txt
if not exist config mkdir config
"%OUT%\xbe_symbols.exe" assets\%GAME_XBE% "%SDK%\xdk" --out config\game.toml || goto :fail

rem game
:game
echo [3/3] Game code
echo       first build recompiles %GAME_XBE%, expect 30 to 90 minutes ...
cmake --build out\build\release || goto :fail

echo.
echo   %GAME_TITLE% built.
echo   out\build\release\%GAME_EXE%.exe
echo.
if defined CI exit /b 0
choice /c YN /n /m "  Play now? [Y/N] "
if not errorlevel 2 (start "" /d "%OUT%" "%OUT%\%GAME_EXE%.exe") else (pause)
exit /b 0

rem helpers
:setuptools
set TOOLS_OK=
set MISSING=
rem VsDevCmd moves to its own start folder unless told, and cmd reads this file again by its name
set "VSCMD_START_DIR=%CD%"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
rem an LLVM elsewhere: set LLVM_DIR to its folder (the one with bin\clang++.exe)
if defined LLVM_DIR if exist "%LLVM_DIR%\bin\clang++.exe" set "PATH=%LLVM_DIR%\bin;%PATH%"
where clang++ >nul 2>nul || set MISSING=%MISSING% clang++
where cmake   >nul 2>nul || set MISSING=%MISSING% cmake
where ninja   >nul 2>nul || set MISSING=%MISSING% ninja
where git     >nul 2>nul || set MISSING=%MISSING% git
if defined MISSING (
    echo       missing:%MISSING%
    exit /b 0
)
set TOOLS_OK=1
exit /b 0

:byhand
echo.
echo       Install by hand, then run this again:
echo         clang++ / cmake / ninja  -  Visual Studio Installer, Modify, Individual
echo                                     components: "C++ Clang tools for Windows"
echo                                     and "C++ CMake tools for Windows"
echo         git                      -  https://git-scm.com/download/win
goto :fail

:nogame
echo   Put the full game files in the assets folder first:
echo     %CD%\assets
echo   It needs %GAME_XBE% with the game's folders beside it.
goto :fail

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
if not defined CI pause
exit /b 1
