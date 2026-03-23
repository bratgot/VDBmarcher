@echo off
REM  VDBRender build script - Nuke 17+
REM  Run from VDBmarcher source dir in x64 Native Tools Command Prompt.
setlocal enabledelayedexpansion

set "SRC=%CD%"
set "VCPKG=C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
set "VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin"
set "DEST=%USERPROFILE%\.nuke\plugins\VDBRender"
set /a N=0

echo.
echo  VDBRender Builder
echo.

for /d %%D in ("C:\Program Files\Nuke*") do call :build "%%~D"

set "MENUPY=%USERPROFILE%\.nuke\menu.py"
if %N% GTR 0 (
    findstr /c:"_load_vdbrender" "%MENUPY%" >nul 2>&1
    if errorlevel 1 (
        echo.>> "%MENUPY%"
        type "%SRC%\VDBRender_menu.py" >> "%MENUPY%"
        echo  Menu block appended to %MENUPY%
    )
)

echo.
echo  Built %N% version(s) to %DEST%
echo.
goto :eof

:build
set "ND=%~1"
set "NN=%~nx1"

if not exist "%ND%\include\DDImage\Iop.h" goto :eof

if not exist "%ND%\tbb12.dll" (
    echo  SKIP %NN% [old TBB]
    goto :eof
)

for /f "tokens=1 delims=." %%V in ("%NN:Nuke=%") do set "MJ=%%V"

echo  BUILD %NN%

set "BD=%SRC%\build_nuke%MJ%"
if exist "%BD%" rmdir /s /q "%BD%"
mkdir "%BD%"
pushd "%BD%"

cmake "%SRC%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE="%VCPKG%" -DNUKE_ROOT="%ND%" -DCMAKE_CXX_STANDARD=20

nmake

if exist "VDBRender.dll" (
    if not exist "%DEST%\nuke%MJ%" mkdir "%DEST%\nuke%MJ%"
    copy /Y VDBRender.dll "%DEST%\nuke%MJ%\" >nul
    for %%F in (openvdb.dll tbb12.dll tbbmalloc.dll Imath-3_2.dll blosc.dll zlib1.dll zstd.dll lz4.dll) do (
        if exist "%VCPKG_BIN%\%%F" copy /Y "%VCPKG_BIN%\%%F" "%DEST%\nuke%MJ%\" >nul
    )
    echo  OK %NN%
    set /a N+=1
) else (
    echo  FAIL %NN%
)

popd
goto :eof
