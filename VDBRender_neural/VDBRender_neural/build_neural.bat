@echo off
setlocal enabledelayedexpansion
:: ─────────────────────────────────────────────────────
:: build_neural.bat — Build VDBRender with NeuralVDB support
:: Run from the VDBmarcher project root after placing
:: NeuralDecoder.h alongside VDBRenderIop.h
:: ─────────────────────────────────────────────────────

set VCPKG=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
set LIBTORCH=C:\libtorch

echo.
echo  ============================================
echo   VDBRender + NeuralVDB Build
echo  ============================================
echo.

:: Check LibTorch
if not exist "%LIBTORCH%\share\cmake\Torch\TorchConfig.cmake" (
    echo  WARNING: LibTorch not found at %LIBTORCH%
    echo  Download from https://pytorch.org ^(C++/cxx11 ABI^)
    echo.
    echo  Building WITHOUT NeuralVDB support...
    set NEURAL_FLAG=OFF
) else (
    echo  LibTorch found: %LIBTORCH%
    set NEURAL_FLAG=ON
)

:: Find Nuke
set NUKE_FOUND=0
for /d %%N in ("C:\Program Files\Nuke*") do (
    if exist "%%N\tbb12.dll" (
        set NUKE_DIR=%%N
        set NUKE_FOUND=1
    )
)

if %NUKE_FOUND%==0 (
    echo  ERROR: No compatible Nuke installation found
    echo  Looking for Nuke with tbb12.dll in C:\Program Files\
    pause
    exit /b 1
)

echo  Nuke: %NUKE_DIR%
echo  Neural: %NEURAL_FLAG%
echo.

:: Build
set BUILD_DIR=build_neural
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%" && cd "%BUILD_DIR%"

cmake .. -G "NMake Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG% ^
    -DNUKE_ROOT="%NUKE_DIR%" ^
    -DVDBRENDER_NEURAL=%NEURAL_FLAG% ^
    -DLIBTORCH_PATH=%LIBTORCH%

if errorlevel 1 (
    echo.
    echo  CMake configuration failed.
    pause
    exit /b 1
)

nmake

if errorlevel 1 (
    echo.
    echo  Build failed.
    pause
    exit /b 1
)

echo.
echo  ============================================
echo   Build successful!
echo   Output: %CD%\VDBRender.dll
if "%NEURAL_FLAG%"=="ON" (
    echo   NeuralVDB: ENABLED
) else (
    echo   NeuralVDB: disabled
)
echo  ============================================
echo.
pause
