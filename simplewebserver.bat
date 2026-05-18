@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "BIN=%SCRIPT_DIR%bin\simplewebserver.exe"
set "PID_FILE=%SCRIPT_DIR%simplewebserver.pid"
set "PORT_FILE=%SCRIPT_DIR%.port"
set "LOG_DIR=%SCRIPT_DIR%logs"
set "HTML_DIR=%SCRIPT_DIR%html"
set "DEFAULT_PORT=8881"

set "MAKE_CMD="
where mingw32-make >nul 2>&1
if %ERRORLEVEL% equ 0 set "MAKE_CMD=mingw32-make"
if "%MAKE_CMD%"=="" where make >nul 2>&1
if "%MAKE_CMD%"=="" if %ERRORLEVEL% equ 0 set "MAKE_CMD=make"
if "%MAKE_CMD%"=="" (
    echo [ERROR] make or mingw32-make not found
    exit /b 1
)

goto :main

:: =============================================================================
::  helpers
:: =============================================================================

:alive
    if "%~1"=="" exit /b 1
    tasklist /FI "PID eq %~1" 2>nul | findstr "%~1" >nul 2>&1
    exit /b

:portpid
    set "found="
    if "%~1"=="" exit /b 0
    for /f "tokens=5" %%a in ('netstat -ano 2^>nul ^| findstr /R ":[%~1][ ]"') do (
        if not defined found set "found=%%a"
    )
    exit /b

:kill
    set "pid=%~1"
    if "!pid!"=="" exit /b 0
    tasklist /FI "PID eq !pid!" 2>nul | findstr "!pid!" >nul 2>&1
    if %ERRORLEVEL% neq 0 exit /b 0
    echo [INFO]  Killing PID=!pid! ...
    taskkill /PID !pid! >nul 2>&1
    set "n=0"
:kill_wait
    timeout /t 1 /nobreak >nul 2>&1
    set /a n+=1
    tasklist /FI "PID eq !pid!" 2>nul | findstr "!pid!" >nul 2>&1
    if %ERRORLEVEL% neq 0 exit /b 0
    if !n! lss 10 goto :kill_wait
    echo [WARN]  Force killing PID=!pid! ...
    taskkill /F /PID !pid! >nul 2>&1
    exit /b

:dirs
    if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
    if not exist "%HTML_DIR%" mkdir "%HTML_DIR%"
    exit /b

:: =============================================================================
::  commands
:: =============================================================================

:cmd_build
    echo [INFO] Building...
    cd /d "%SCRIPT_DIR%"
    call %MAKE_CMD% clean
    call %MAKE_CMD%
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Build failed
        exit /b 1
    )
    if not exist "%BIN%" (
        echo [ERROR] Binary not found: %BIN%
        exit /b 1
    )
    echo [OK]    Build done: %BIN%
    exit /b

:cmd_start
    if exist "%PID_FILE%" (
        set /p SPID=<"%PID_FILE%"
        call :alive !SPID!
        if !ERRORLEVEL! equ 0 (
            echo [WARN] Server already running (PID=!SPID!^). Stop it first.
            exit /b 0
        )
        echo [INFO] Stale PID file removed
        del "%PID_FILE%" 2>nul
    )

    if not exist "%BIN%" (
        echo [INFO] Binary not found, building...
        call :cmd_build
        if !ERRORLEVEL! neq 0 exit /b 1
    )

    call :dirs

    set "PORT=%DEFAULT_PORT%"
    set "next="
    for %%a in (%*) do (
        if "!next!"=="1" ( set "PORT=%%a" & set "next=" )
        if "%%a"=="-p" set "next=1"
    )
    echo !PORT!>"%PORT_FILE%"

    echo [INFO] Starting simplewebserver on port !PORT!...
    start "simplewebserver" /B "%BIN%" %*

    timeout /t 2 /nobreak >nul 2>&1
    call :portpid !PORT!
    if defined found (
        echo !found!>"%PID_FILE%"
        call :alive !found!
        if !ERRORLEVEL! equ 0 (
            echo [OK]    Server started (PID=!found!)
            echo [OK]    URL:  http://localhost:!PORT!
            echo [OK]    Logs: http://localhost:!PORT!/logviewer.html
            exit /b 0
        )
    )
    echo [ERROR] Failed to start. Try running directly:
    echo         "%BIN%" -p !PORT!
    del "%PID_FILE%" 2>nul
    exit /b 1

:cmd_stop
    set "DONE=0"

    if exist "%PID_FILE%" (
        set /p SPID=<"%PID_FILE%"
        call :alive !SPID!
        if !ERRORLEVEL! equ 0 (
            echo [INFO] Stopping server (PID=!SPID!^)...
            call :kill !SPID!
            echo [OK]    Server stopped (PID=!SPID!^)
            set "DONE=1"
        )
        del "%PID_FILE%" 2>nul
    )

    set "PV=%DEFAULT_PORT%"
    if exist "%PORT_FILE%" set /p PV=<"%PORT_FILE%"
    call :portpid !PV!
    if defined found (
        echo [WARN] Port !PV! still in use by PID=!found!. Killing...
        call :kill !found!
        echo [OK]    Process on port !PV! killed
        set "DONE=1"
    )

    if "!DONE!"=="0" echo [WARN] Server is not running
    exit /b 0

:cmd_restart
    echo [INFO] Restarting server...
    call :cmd_stop
    call :cmd_build
    call :cmd_start %*
    exit /b

:cmd_status
    echo ==================================================
    echo   simplewebserver Status
    echo ==================================================
    set "P="
    if exist "%PID_FILE%" set /p P=<"%PID_FILE%"
    if defined P (
        call :alive !P!
        if !ERRORLEVEL! equ 0 (
            echo   Status:  RUNNING (PID=!P!^)
            for /f "tokens=5" %%m in ('tasklist /FI "PID eq !P!" /FO TABLE 2^>nul ^| findstr "[0-9]"') do (
                echo   Memory:  %%m
            )
            goto :status_url
        )
        del "%PID_FILE%" 2>nul
    )
    set "PV=%DEFAULT_PORT%"
    if exist "%PORT_FILE%" set /p PV=<"%PORT_FILE%"
    call :portpid !PV!
    if defined found (
        echo   Status:  RUNNING (port !PV!, PID=!found!^)
    ) else (
        echo   Status:  NOT RUNNING
    )
:status_url
    set "PV=%DEFAULT_PORT%"
    if exist "%PORT_FILE%" set /p PV=<"%PORT_FILE%"
    echo   URL:     http://localhost:!PV!
    echo ==================================================
    exit /b

:cmd_help
    echo.
    echo   Usage: simplewebserver.bat ^<command^> [options]
    echo.
    echo   Commands:
    echo     build              Compile only
    echo     start  [options]   Start server (auto-build if needed)
    echo     stop               Stop server
    echo     restart [options]  Rebuild and restart
    echo     status             Show running status
    echo     help               Show this help
    echo.
    echo   Options (start/restart):
    echo     -p ^<port^>          Default: 8881
    echo     -t ^<threads^>       Default: auto
    echo     -q ^<size^>          Default: 128
    echo     -l ^<dir^>           Default: logs
    echo.
    echo   Examples:
    echo     simplewebserver.bat start
    echo     simplewebserver.bat start -p 9000
    echo     simplewebserver.bat restart
    echo     simplewebserver.bat status
    echo     simplewebserver.bat stop
    echo.
    echo   Binary:  %BIN%
    echo   Logs:    %LOG_DIR%
    echo   Make:    %MAKE_CMD%
    exit /b

:: =============================================================================
::  main entry
:: =============================================================================

:main
if "%~1"=="" goto :cmd_help

set "CMD=%~1"
shift

if /I "%CMD%"=="build"    call :cmd_build   & goto :eof
if /I "%CMD%"=="start"    call :cmd_start %1 %2 %3 %4 %5 %6 %7 %8 %9 & goto :eof
if /I "%CMD%"=="stop"     call :cmd_stop    & goto :eof
if /I "%CMD%"=="restart"  call :cmd_restart %1 %2 %3 %4 %5 %6 %7 %8 %9 & goto :eof
if /I "%CMD%"=="status"   call :cmd_status  & goto :eof
if /I "%CMD%"=="help"     call :cmd_help    & goto :eof
if /I "%CMD%"=="--help"   call :cmd_help    & goto :eof
if /I "%CMD%"=="-h"       call :cmd_help    & goto :eof

echo [ERROR] Unknown command: %CMD%
call :cmd_help
exit /b 1
