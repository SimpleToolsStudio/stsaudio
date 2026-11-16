@echo off
setlocal EnableDelayedExpansion

echo Searching for Python 3.14 in C drive...
set "PYTHON_EXE=C:\Program Files\Python314\python.exe"

if not exist "!PYTHON_EXE!" (
    set "PYTHON_EXE=C:\Python314\python.exe"
)

if not exist "!PYTHON_EXE!" (
    echo [ERROR] Python 3.14 not found in C drive.
    echo Downloading EXE for Python 3.14...
    start https://www.python.org/ftp/python/3.14.10/python-3.14.10-amd64.exe
    goto :fail
)

echo Selected Python: "!PYTHON_EXE!"

if not exist audio.c       ( echo [ERROR] audio.c not found.       & goto :fail )
if not exist miniaudio.h   ( echo [ERROR] miniaudio.h not found.   & goto :fail )
if not exist minimp3.h     ( echo [ERROR] minimp3.h not found.     & goto :fail )
if not exist minimp3_ex.h  ( echo [ERROR] minimp3_ex.h not found.  & goto :fail )

set "PY_SCRIPT=%TEMP%\pyconfig.py"
>"!PY_SCRIPT!" echo(import sysconfig
>>"!PY_SCRIPT!" echo(print(sysconfig.get_paths()['include'])
>>"!PY_SCRIPT!" echo(print(sysconfig.get_config_var('installed_base') + '\\libs')

set "LINE_NUM=0"
for /f "usebackq delims=" %%i in (`""!PYTHON_EXE!" "!PY_SCRIPT!""`) do (
    set /a LINE_NUM+=1
    if !LINE_NUM! EQU 1 set "PY_INC=%%i"
    if !LINE_NUM! EQU 2 set "PY_LIBDIR=%%i"
)
del "!PY_SCRIPT!" >nul 2>nul

if not defined PY_INC (
    echo [ERROR] Failed to query Python configuration.
    goto :fail
)

set "PY_LIBNAME=python314.lib"
if not exist "!PY_LIBDIR!\!PY_LIBNAME!" (
    echo [ERROR] python314.lib not found in "!PY_LIBDIR!"
    goto :fail
)

set "OUTPUT_DLL=audio.pyd"

echo ====================================================
echo   Compiler  : MSVC (cl.exe)
echo   Python    : "!PYTHON_EXE!"
echo   Include   : !PY_INC!
echo   LibDir    : !PY_LIBDIR!
echo   Implib    : !PY_LIBNAME!
echo   Output    : !OUTPUT_DLL!
echo ====================================================

cl /nologo /O2 /MD /W3 /c /I"!PY_INC!" audio.c
if errorlevel 1 goto :fail

link /nologo /DLL /RELEASE /OPT:REF /OPT:ICF /OUT:!OUTPUT_DLL! audio.obj /LIBPATH:"!PY_LIBDIR!" !PY_LIBNAME! ole32.lib winmm.lib user32.lib dxguid.lib uuid.lib
if errorlevel 1 goto :fail

echo.
echo BUILD OK: !OUTPUT_DLL!
echo.
echo Test:
echo   "!PYTHON_EXE!" -c "import audio; print(audio.version())"
echo.
pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1