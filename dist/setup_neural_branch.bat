@echo off
setlocal enabledelayedexpansion
:: ─────────────────────────────────────────────────────
:: setup_neural_branch.bat
:: Creates the feature/neural-vdb branch and stages all
:: neural integration files. Run once from C:\dev\VDBmarcher
::
:: Usage:
::   1. Extract VDBRender_neural.tar.gz somewhere
::   2. Copy this script + all neural files into C:\dev\VDBmarcher
::      OR set NEURAL_SRC below to the extracted folder
::   3. Run this script from C:\dev\VDBmarcher
:: ─────────────────────────────────────────────────────

set REPO=C:\dev\VDBmarcher
set BRANCH=feature/neural-vdb

:: Source folder — set to wherever you extracted the neural files
:: If the files are already in the repo root, leave as %REPO%
set NEURAL_SRC=%REPO%

echo.
echo  ============================================
echo   VDBRender — NeuralVDB Branch Setup
echo  ============================================
echo.

:: ── Check we're in the right place ────────────────────
cd /d "%REPO%" || (
    echo  ERROR: Cannot cd to %REPO%
    pause
    exit /b 1
)

if not exist ".git" (
    echo  ERROR: %REPO% is not a git repository.
    echo  Run: git init ^&^& git remote add origin https://github.com/bratgot/VDBmarcher.git
    pause
    exit /b 1
)

:: ── Stash any uncommitted work ────────────────────────
echo [1/7] Stashing uncommitted changes...
git stash --include-untracked >nul 2>&1
echo       Done.

:: ── Make sure we're on main/master ────────────────────
echo [2/7] Checking current branch...
for /f "tokens=*" %%b in ('git branch --show-current') do set CURRENT=%%b
echo       Currently on: %CURRENT%

:: ── Create the feature branch ─────────────────────────
echo [3/7] Creating branch: %BRANCH%
git branch %BRANCH% 2>nul
if errorlevel 1 (
    echo       Branch already exists — switching to it.
)
git checkout %BRANCH%
echo       On branch: %BRANCH%

:: ── Copy neural files into the repo ───────────────────
echo [4/7] Copying neural integration files...

:: Header — replaces existing VDBRenderIop.h with neural-augmented version
if exist "%NEURAL_SRC%\VDBRenderIop.h" (
    copy /Y "%NEURAL_SRC%\VDBRenderIop.h" "%REPO%\VDBRenderIop.h" >nul
    echo       Updated: VDBRenderIop.h
)

:: New files
for %%F in (
    NeuralDecoder.h
    INTEGRATION.md
    build_neural.bat
    nvdb_encoder.py
    nvdb_validate.py
    test_synthetic.py
    requirements.txt
) do (
    if exist "%NEURAL_SRC%\%%F" (
        copy /Y "%NEURAL_SRC%\%%F" "%REPO%\%%F" >nul
        echo       Added:   %%F
    ) else (
        echo       SKIP:    %%F not found in %NEURAL_SRC%
    )
)

:: CMakeLists.txt — replace with neural-enabled version
if exist "%NEURAL_SRC%\CMakeLists.txt" (
    copy /Y "%NEURAL_SRC%\CMakeLists.txt" "%REPO%\CMakeLists.txt" >nul
    echo       Updated: CMakeLists.txt
)

:: Create python/ subfolder for encoder tools
if not exist "%REPO%\python" mkdir "%REPO%\python"
for %%F in (nvdb_encoder.py nvdb_validate.py test_synthetic.py requirements.txt) do (
    if exist "%REPO%\%%F" (
        copy /Y "%REPO%\%%F" "%REPO%\python\%%F" >nul
    )
)
echo       Mirrored python tools to python\

:: ── Update .gitignore ─────────────────────────────────
echo [5/7] Updating .gitignore...
set NEEDS_IGNORE=0
if not exist "%REPO%\.gitignore" set NEEDS_IGNORE=1
if %NEEDS_IGNORE%==0 (
    findstr /C:"build_neural" "%REPO%\.gitignore" >nul 2>&1
    if errorlevel 1 set NEEDS_IGNORE=1
)
if %NEEDS_IGNORE%==1 (
    echo.>> "%REPO%\.gitignore"
    echo # NeuralVDB build>> "%REPO%\.gitignore"
    echo build_neural/>> "%REPO%\.gitignore"
    echo *.nvdb>> "%REPO%\.gitignore"
    echo __pycache__/>> "%REPO%\.gitignore"
    echo *.pyc>> "%REPO%\.gitignore"
    echo       Updated .gitignore
) else (
    echo       Already up to date.
)

:: ── Stage everything ──────────────────────────────────
echo [6/7] Staging files...
git add -A
echo       Staged.

:: ── Commit ────────────────────────────────────────────
echo [7/7] Committing...
git commit -m "feat: NeuralVDB integration — neural compressed volume support" ^
           -m "Adds .nvdb file support alongside standard .vdb files." ^
           -m "" ^
           -m "New files:" ^
           -m "  NeuralDecoder.h      — neural decoder (header-only, ifdef-guarded)" ^
           -m "  INTEGRATION.md       — exact code changes for VDBRenderIop.cpp" ^
           -m "  build_neural.bat     — build script with LibTorch auto-detect" ^
           -m "  nvdb_encoder.py      — VDB to NVDB training script" ^
           -m "  nvdb_validate.py     — compression quality validation" ^
           -m "  test_synthetic.py    — end-to-end test (no real VDB needed)" ^
           -m "" ^
           -m "Modified files:" ^
           -m "  VDBRenderIop.h       — MarchCtx::sampleDensity() abstraction" ^
           -m "  CMakeLists.txt       — optional -DVDBRENDER_NEURAL=ON" ^
           -m "" ^
           -m "Build without LibTorch: unchanged, zero overhead." ^
           -m "Build with LibTorch: -DVDBRENDER_NEURAL=ON -DLIBTORCH_PATH=C:/libtorch"

echo.
echo  ============================================
echo   Branch created: %BRANCH%
echo.
echo   Next steps:
echo     1. Open INTEGRATION.md
echo     2. Apply the find/replace changes to VDBRenderIop.cpp
echo     3. Run build_neural.bat to compile
echo     4. Test with: python test_synthetic.py
echo.
echo   To push:
echo     git push -u origin %BRANCH%
echo.
echo   To switch back:
echo     git checkout %CURRENT%
echo     git stash pop
echo  ============================================
echo.
pause
