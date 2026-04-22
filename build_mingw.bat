@echo off
REM MinGW-w64: 与 build_win.ps1 相同，设置 OS=Windows_NT 以启用 Makefile 的 Windows 分支与 -lws2_32
setlocal
set OS=Windows_NT
where mingw32-make >nul 2>&1 && mingw32-make %* && exit /b %ERRORLEVEL%
where make >nul 2>&1 && make %* && exit /b %ERRORLEVEL%
echo [提示] 未找到 mingw32-make。可执行: winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
echo 安装后重新打开终端，或将 MinGW 的 bin 加入 PATH。
exit /b 1
