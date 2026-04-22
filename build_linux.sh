#!/usr/bin/env sh
# Linux / macOS / WSL: 在 POSIX shell 下编译（不设置 OS=Windows_NT，以使用 monitor.c + ssh_exec.c）
set -e
cd "$(dirname "$0")"
unset OS 2>/dev/null || true
export OS="${OS-}"
# 与 Git for Windows 不同，在 Linux 上一般不应设置 Windows_NT
case "${OS:-}" in
  Windows_NT)
    echo "Detected OS=Windows_NT. For MinGW/Windows 请用 build_mingw.bat 或 build_win.ps1" >&2
    exit 1
    ;;
esac
if ! command -v make >/dev/null 2>&1; then
  echo "需要 GNU make" >&2
  exit 1
fi
make "$@"
