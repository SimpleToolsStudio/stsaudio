@echo off
setlocal EnableDelayedExpansion

rem ==============================================================================
rem Auto-detect 64-bit standard Python (Avoid free-threaded 't' builds)
rem ==============================================================================
echo Searching for 64-bit Python...
set "PYTHON="

rem Try python first, then python3, then py launcher
for %%P in (python python3) do (
    if not defined PYTHON (
        for /f "delims=" %%i in ('%%P -c "import sys,struct; print(struct.calcsize('P')*8)" 2^>nul') do (
            if "%%i"=="64" set "PYTHON=%%P"
        )
    )
)

rem If not found via PATH, try py launcher (force standard 64-bit)
if not defined PYTHON (
    for /f "delims=" %%i in ('py -3-64 -c "import sys,struct; print(struct.calcsize('P')*8)" 2^>nul') do (
        if "%%i"=="64" set "PYTHON=py -3-64"
    )
)

if not defined PYTHON (
    echo [ERROR] No 64-bit Python found.
    echo Please install Python 64-bit and add it to your PATH.
    goto :fail
)
echo Using Python: %PYTHON%

rem --- Check for required source files ---
if not exist audio.c       ( echo [ERROR] audio.c not found.       & goto :fail )
if not exist miniaudio.h   ( echo [ERROR] miniaudio.h not found.   & goto :fail )
if not exist minimp3.h     ( echo [ERROR] minimp3.h not found.     & goto :fail )
if not exist minimp3_ex.h  ( echo [ERROR] minimp3_ex.h not found.  & goto :fail )

rem ==============================================================================
rem Query configuration via temp script (avoids all quoting issues)
rem ==============================================================================
echo Querying Python configuration...

>"%TEMP%\pyconfig.py" echo import sysconfig
>>"%TEMP%\pyconfig.py" echo print(sysconfig.get_paths()['include'])
>>"%TEMP%\pyconfig.py" echo print(sysconfig.get_config_var('LIBDIR') or '')

set "LINE_NUM=0"
for /f "usebackq delims=" %%i in (`%PYTHON% "%TEMP%\pyconfig.py"`) do (
    set /a LINE_NUM+=1
    if !LINE_NUM! EQU 1 set "PY_INC=%%i"
    if !LINE_NUM! EQU 2 set "PY_LIBDIR=%%i"
)
del "%TEMP%\pyconfig.py" >nul 2>nul

rem --- Fallback for LIBDIR if empty ---
if not defined PY_LIBDIR (
    for /f "usebackq delims=" %%i in (`%PYTHON% -c "import sys,os; print(os.path.join(sys.base_prefix, 'libs'))"`) do set "PY_LIBDIR=%%i"
)

rem ==============================================================================
rem Determine Python import library
rem ==============================================================================
set "PY_IMPLIB="
for %%f in ("%PY_LIBDIR%\python3*.lib" "%PY_LIBDIR%\libpython3*.dll.a") do (
    if not defined PY_IMPLIB if exist "%%~f" set "PY_IMPLIB=%%~f"
)

if not defined PY_IMPLIB (
    echo [ERROR] Python import library not found in "%PY_LIBDIR%"
    goto :fail
)

rem ==============================================================================
rem Output filename is strictly 'audio.pyd'
rem ==============================================================================
set "OUTPUT_DLL=audio.pyd"

echo ====================================================
echo   Python    : %PYTHON%
echo   Include   : %PY_INC%
echo   LibDir    : %PY_LIBDIR%
echo   Implib    : %PY_IMPLIB%
echo   Output    : %OUTPUT_DLL%
echo ====================================================

rem ==============================================================================
rem Build
rem ==============================================================================
echo Building %OUTPUT_DLL%...
gcc -shared -O2 -s -Wno-stringop-overflow -o "%OUTPUT_DLL%" audio.c ^
    -I"." -I"%PY_INC%" ^
    -static-libgcc -static-libstdc++ ^
    "%PY_IMPLIB%" ^
    -lole32 -lwinmm -luser32 -ldxguid -luuid

if errorlevel 1 goto :fail

echo.
echo BUILD OK: %OUTPUT_DLL%
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