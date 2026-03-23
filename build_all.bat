@echo off
REM  VDBRender — Build for all installed Nuke versions
REM  Run from VDBmarcher source dir in x64 Native Tools Command Prompt.
setlocal enabledelayedexpansion

set "SRC=%CD%"
set "VCPKG=C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
set "VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin"
set "DEST=%USERPROFILE%\.nuke\plugins\VDBRender"
set /a N=0

echo.
echo  VDBRender Multi-Version Builder
echo.

for /d %%D in ("C:\Program Files\Nuke*") do (
    set "ND=%%~D"
    set "NN=%%~nxD"
    for /f "tokens=1 delims=." %%V in ("!NN:Nuke=!") do set "MJ=%%V"

    if exist "!ND!\include\DDImage\Iop.h" (
        echo  ---- !NN! ----

        set STD=20
        if !MJ! LEQ 15 set STD=17

        set "BD=!SRC!\build_nuke!MJ!"
        if exist "!BD!" rmdir /s /q "!BD!"
        mkdir "!BD!"
        pushd "!BD!"

        cmake "!SRC!" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE="!VCPKG!" -DNUKE_ROOT="!ND!" -DCMAKE_CXX_STANDARD=!STD!

        nmake

        if exist "VDBRender.dll" (
            if not exist "!DEST!\nuke!MJ!" mkdir "!DEST!\nuke!MJ!"
            copy /Y VDBRender.dll "!DEST!\nuke!MJ!\" >nul

            REM Copy vcpkg dependency DLLs alongside the plugin
            for %%F in (openvdb.dll tbb12.dll tbbmalloc.dll Imath-3_2.dll blosc.dll zlib1.dll) do (
                if exist "!VCPKG_BIN!\%%F" copy /Y "!VCPKG_BIN!\%%F" "!DEST!\nuke!MJ!\" >nul
            )

            echo  [OK] !NN! -^> nuke!MJ!\
            set /a N+=1
        ) else (
            echo  [FAIL] !NN!
        )
        popd
        echo.
    )
)

REM Append VDBRender menu snippet to menu.py (never overwrite)
set "MENUPY=%USERPROFILE%\.nuke\menu.py"
if !N! GTR 0 (
    findstr /c:"_load_vdbrender" "!MENUPY!" >nul 2>&1
    if errorlevel 1 (
        echo.>> "!MENUPY!"
        type "!SRC!\VDBRender_menu.py" >> "!MENUPY!"
        echo  VDBRender block appended to !MENUPY!
    ) else (
        echo  VDBRender block already in !MENUPY! — skipped.
    )
)

echo.
echo  Done: !N! version(s) built to %DEST%
echo.
endlocal
