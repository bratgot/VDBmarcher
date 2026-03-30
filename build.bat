@echo off
setlocal EnableDelayedExpansion

set BUILD_TYPE=Release
set TARGET=all
set LOG_FILE=%~dp0build\build_log.txt
set ERROR_FILE=%~dp0build\build_errors.txt
set NUKE_ROOT=C:\Program Files\Nuke17.0v1
set VCPKG=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
set VCPKG_PREFIX=C:\vcpkg\installed\x64-windows
set VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin
set PLUGIN_DIR=%USERPROFILE%\.nuke\plugins\VDBRender\nuke17
set PLUGIN_DIR_N17=%USERPROFILE%\.nuke\plugins\VDBRender\nuke17

if /i "%1"=="debug"     set BUILD_TYPE=Debug
if /i "%1"=="release"   set BUILD_TYPE=Release
if /i "%2"=="configure" set TARGET=configure
if /i "%2"=="build"     set TARGET=build
if /i "%2"=="install"   set TARGET=install
if /i "%2"=="dist"      set TARGET=dist

mkdir "%~dp0build" 2>nul
echo. > "%LOG_FILE%"
echo. > "%ERROR_FILE%"

echo.
echo  VDBRender  [%BUILD_TYPE%]  %DATE% %TIME%
echo  ================================================

:: ── Configure ────────────────────────────────────────────────────────────────
if /i "%TARGET%"=="build"   goto :do_build
if /i "%TARGET%"=="install" goto :do_install
if /i "%TARGET%"=="dist"    goto :do_dist

echo  [1/3] Configuring...
cmake -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG%" ^
    -DCMAKE_PREFIX_PATH="%VCPKG_PREFIX%" ^
    -DNUKE_ROOT="%NUKE_ROOT%" ^
    >> "%LOG_FILE%" 2>&1
call :check_step "Configure" || goto :print_summary

:: ── Build ────────────────────────────────────────────────────────────────────
:do_build
echo  [2/3] Building...
cmake --build "%~dp0build" --config %BUILD_TYPE% -j %NUMBER_OF_PROCESSORS% ^
    >> "%LOG_FILE%" 2>&1
call :check_step "Build" || goto :print_summary
if /i "%TARGET%"=="build" goto :print_summary

:: ── Install — manual xcopy, no cmake install ─────────────────────────────────
:do_install
echo  [3/3] Installing to %PLUGIN_DIR%...
mkdir "%PLUGIN_DIR%" 2>nul

:: DLL
copy /Y "%~dp0build\%BUILD_TYPE%\VDBRender.dll" "%PLUGIN_DIR%\" >> "%LOG_FILE%" 2>&1
call :check_step "Copy VDBRender.dll" || goto :print_summary
:: Also copy to nuke17 subfolder (legacy install path)
mkdir "%PLUGIN_DIR_N17%" 2>nul
copy /Y "%~dp0build\%BUILD_TYPE%\VDBRender.dll" "%PLUGIN_DIR_N17%\" >> "%LOG_FILE%" 2>&1

:: Menu script
copy /Y "%~dp0VDBRender_menu.py" "%PLUGIN_DIR%\" >> "%LOG_FILE%" 2>&1

:: Runtime DLLs
for %%F in (openvdb.dll Imath-3_2.dll tbb12.dll blosc.dll zlib1.dll zstd.dll lz4.dll) do (
    if exist "%VCPKG_BIN%\%%F" (
        copy /Y "%VCPKG_BIN%\%%F" "%PLUGIN_DIR%\" >> "%LOG_FILE%" 2>&1
    )
)

echo  [OK]   Installed to %PLUGIN_DIR%
echo.
dir "%PLUGIN_DIR%\VDBRender.dll" | findstr VDBRender
echo.
pause
goto :print_summary

:: ── Dist ─────────────────────────────────────────────────────────────────────
:do_dist
echo  [3/3] Building dist package...
cmake --build "%~dp0build" --config %BUILD_TYPE% --target dist >> "%LOG_FILE%" 2>&1
call :check_step "Dist" || goto :print_summary
echo        Output: %~dp0dist\VDBRender_v3\
goto :print_summary

:: ── Summary ──────────────────────────────────────────────────────────────────
:print_summary
echo.
echo  ================================================
echo  Summary

findstr /R "error C[0-9][0-9][0-9][0-9]"   "%LOG_FILE%" >  "%ERROR_FILE%" 2>nul
findstr /R "error LNK[0-9][0-9][0-9][0-9]" "%LOG_FILE%" >> "%ERROR_FILE%" 2>nul
findstr /R "fatal error C"                  "%LOG_FILE%" >> "%ERROR_FILE%" 2>nul

set /a ERR=0
set /a WARN=0
for /f %%N in ('type "%ERROR_FILE%" 2^>nul ^| find /c /v ""') do set ERR=%%N
for /f %%N in ('findstr /R "warning C[0-9][0-9][0-9][0-9]" "%LOG_FILE%" 2^>nul ^| find /c /v ""') do set WARN=%%N

if %ERR% GTR 0 (
    echo.
    echo  ERRORS  [%ERR%]:
    echo  ------------------------------------------------
    for /f "usebackq tokens=*" %%L in ("%ERROR_FILE%") do (
        set "LINE=%%L"
        set "LINE=!LINE: [C:\dev\VDBmarcher\build\VDBRender.vcxproj]=!"
        for /f "tokens=1* delims=)" %%A in ("!LINE!") do (
            set "LOC=%%A"
            set "MSG=%%B"
        )
        for %%F in ("!LOC!") do set "SHORT=%%~nxF"
        if defined MSG (
            echo    !SHORT!) !MSG!
        ) else (
            echo    !LINE!
        )
    )
    echo.
    echo  WARNINGS: %WARN%
    echo  Full log:  build\build_log.txt
    echo  Error log: build\build_errors.txt
    echo.
    pause
    exit /b 1
) else (
    if %WARN% GTR 0 (
        echo  Warnings: %WARN%  ^(see build\build_log.txt^)
    ) else (
        echo  No errors or warnings.
    )
    echo  Full log: build\build_log.txt
    echo.
    exit /b 0
)

:check_step
set STEP=%~1
if errorlevel 1 (
    echo  [FAIL] %STEP% failed.
    exit /b 1
) else (
    echo  [OK]   %STEP% done.
    exit /b 0
)
