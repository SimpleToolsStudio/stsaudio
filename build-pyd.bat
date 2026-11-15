@echo off
setlocal EnableDelayedExpansion

rem --- Configuration ---
set "PYTHON=python"
set "PY_EXT=.cp314-mingw_x86_64_msvcrt_gnu.pyd"

rem --- Check for required source files ---
if not exist audio.c       ( echo [ERROR] audio.c not found.       & goto :fail )
if not exist miniaudio.h   ( echo [ERROR] miniaudio.h not found.   & goto :fail )
if not exist minimp3.h     ( echo [ERROR] minimp3.h not found.     & goto :fail )
if not exist minimp3_ex.h  ( echo [ERROR] minimp3_ex.h not found.  & goto :fail )

rem --- Query Python include and lib dirs automatically ---
echo Querying Python configuration...
for /f "usebackq delims=" %%i in (`%PYTHON% -c "import sysconfig; print(sysconfig.get_paths()['include'])"`) do set "PY_INC=%%i"
for /f "usebackq delims=" %%i in (`%PYTHON% -c "import sysconfig; print(sysconfig.get_config_var('LIBDIR'))"`) do set "PY_LIBDIR=%%i"
for /f "usebackq delims=" %%i in (`%PYTHON% -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"`) do set "PY_EXT=%%i"

rem --- Fallback for LIBDIR if None ---
if "%PY_LIBDIR%"=="None" (
    for /f "usebackq delims=" %%i in (`%PYTHON% -c "import sys,os; print(os.path.join(sys.base_prefix, 'libs'))"`) do set "PY_LIBDIR=%%i"
)

rem --- Determine Python import library ---
set "PY_IMPLIB="
if exist "%PY_LIBDIR%\libpython3.14.dll.a" set "PY_IMPLIB=%PY_LIBDIR%\libpython3.14.dll.a"
if exist "%PY_LIBDIR%\python3.14.lib"      set "PY_IMPLIB=%PY_LIBDIR%\python3.14.lib"
if exist "%PY_LIBDIR%\libpython314.dll.a"  set "PY_IMPLIB=%PY_LIBDIR%\libpython314.dll.a"
if exist "%PY_LIBDIR%\python314.lib"       set "PY_IMPLIB=%PY_LIBDIR%\python314.lib"

if "%PY_IMPLIB%"=="" (
    echo [ERROR] Python import library not found in %PY_LIBDIR%
    goto :fail
)

echo ====================================================
echo   Python    : %PYTHON%
echo   Include   : %PY_INC%
echo   LibDir    : %PY_LIBDIR%
echo   Implib    : %PY_IMPLIB%
echo   EXT_SUFFIX: %PY_EXT%
echo ====================================================

rem --- Build ---
echo Building %PY_EXT%...
gcc -shared -O2 -static -Wno-stringop-overflow -o "audio%PY_EXT%" audio.c ^
    -I"." -I"%PY_INC%" ^
    "%PY_IMPLIB%" ^
    -lole32 -lwinmm -luser32 -ldxguid -luuid

if errorlevel 1 goto :fail

echo.
echo BUILD OK: audio%PY_EXT%
echo.
echo Test:
echo   %PYTHON% -c "import audio; print(audio.version())"
echo.
pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1