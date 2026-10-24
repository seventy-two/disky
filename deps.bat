@echo off
rem Builds the libraries voice needs into deps\lib and deps\include: Opus,
rem and Discord's libdave (end-to-end encryption) with mlspp and OpenSSL.
rem Run once before build.bat; it takes a while the first time. Needs
rem Visual Studio 2022 with the C++ workload (for CMake) and Git.
setlocal
cd /d "%~dp0"
set LIBDAVE_COMMIT=6396c90aedbb9a6ee117ea9ce1ce9832af6a3375

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio with C++ tools not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist deps\src mkdir deps\src
if not exist deps\src\libdave (
    git clone https://github.com/discord/libdave deps\src\libdave || exit /b 1
)
git -C deps\src\libdave fetch --quiet origin %LIBDAVE_COMMIT% & git -C deps\src\libdave checkout --quiet %LIBDAVE_COMMIT% || exit /b 1
git -C deps\src\libdave submodule update --init --recursive || exit /b 1
if not exist deps\src\libdave\cpp\vcpkg\vcpkg.exe (
    call deps\src\libdave\cpp\vcpkg\bootstrap-vcpkg.bat -disableMetrics || exit /b 1
)

rem libdave, with its dependencies (and Opus) from vcpkg, from deps\vcpkg.json.
cmake -S deps\src\libdave\cpp -B deps\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=%CD%\deps\src\libdave\cpp\vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_MANIFEST_DIR=%CD%\deps -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DBUILD_SHARED_LIBS=OFF || exit /b 1
cmake --build deps\build --target libdave || exit /b 1

if not exist deps\lib mkdir deps\lib
if not exist deps\include\dave mkdir deps\include\dave
copy /y deps\build\libdave.lib deps\lib\ >nul || exit /b 1
copy /y deps\build\vcpkg_installed\x64-windows-static\lib\*.lib deps\lib\ >nul || exit /b 1
copy /y deps\src\libdave\cpp\includes\dave\dave.h deps\include\dave\ >nul || exit /b 1
xcopy /y /e /i /q deps\build\vcpkg_installed\x64-windows-static\include\opus deps\include\opus >nul || exit /b 1
echo Built deps\lib
