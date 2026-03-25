@echo off
setlocal enabledelayedexpansion
:: ─────────────────────────────────────────────────────
:: make_dist.bat — Assemble VDBRender distribution zip
:: Run from C:\dev\VDBmarcher after build_all.bat
:: ─────────────────────────────────────────────────────

set VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin
set BUILD=build_nuke17
set DIST=dist\VDBRender-2.1-nuke17-win
set NUKE17=%DIST%\nuke17

echo.
echo  Assembling VDBRender distribution...
echo.

:: Clean
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%NUKE17%"

:: Plugin DLL
if not exist "%BUILD%\VDBRender.dll" (
    echo  ERROR: %BUILD%\VDBRender.dll not found.
    echo  Run build_all.bat first.
    pause
    exit /b 1
)
copy /Y "%BUILD%\VDBRender.dll" "%NUKE17%\" >nul
echo  Copied VDBRender.dll

:: Dependency DLLs from vcpkg
set DEPS=openvdb.dll tbb12.dll tbbmalloc.dll Imath-3_2.dll blosc.dll zlib1.dll zstd.dll lz4.dll
set MISSING=0
for %%d in (%DEPS%) do (
    if exist "%VCPKG_BIN%\%%d" (
        copy /Y "%VCPKG_BIN%\%%d" "%NUKE17%\" >nul
        echo  Copied %%d
    ) else (
        echo  WARNING: %VCPKG_BIN%\%%d not found
        set MISSING=1
    )
)
if %MISSING%==1 (
    echo.
    echo  Some dependencies missing. Check vcpkg installation.
    echo  Continuing anyway...
)

:: Text files
copy /Y "VDBRender_menu.py"   "%DIST%\" >nul
copy /Y "LICENSE"              "%DIST%\" >nul

:: Copy dist support files (install scripts, readme, licenses)
:: These live in the dist/ template folder
if exist "dist\template\install.bat"              copy /Y "dist\template\install.bat"              "%DIST%\" >nul
if exist "dist\template\uninstall.bat"            copy /Y "dist\template\uninstall.bat"            "%DIST%\" >nul
if exist "dist\template\README_INSTALL.txt"       copy /Y "dist\template\README_INSTALL.txt"       "%DIST%\" >nul
if exist "dist\template\THIRD_PARTY_LICENSES.txt" copy /Y "dist\template\THIRD_PARTY_LICENSES.txt" "%DIST%\" >nul

:: Count files
set COUNT=0
for /r "%DIST%" %%f in (*) do set /a COUNT+=1

echo.
echo  ─────────────────────────────────────────
echo  Distribution assembled: %DIST%\
echo  %COUNT% files total
echo.
echo  To create zip:
echo    cd dist
echo    tar -a -cf VDBRender-2.1-nuke17-win.zip VDBRender-2.1-nuke17-win
echo  ─────────────────────────────────────────
echo.
pause
