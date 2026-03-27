#!/usr/bin/env bash
# =============================================================================
#  simplewebserver 管理脚本
#  用法: ./server.sh {start|stop|restart|status|build} [选项]
#
#  start 支持的选项（均为可选）：
#    -p <port>      监听端口         (默认: 8081)
#    -t <threads>   工作线程数       (默认: 自动)
#    -q <size>      任务队列长度     (默认: 128)
#    -l <dir>       日志目录         (默认: logs)
# =============================================================================

set -euo pipefail

# ── 路径配置 ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/bin/simpleserver"
PID_FILE="${SCRIPT_DIR}/simpleserver.pid"
LOG_DIR="${SCRIPT_DIR}/logs"
HTML_DIR="${SCRIPT_DIR}/html"
NOHUP_OUT="${LOG_DIR}/nohup.out"

# ── 颜色输出 ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}[INFO]${RESET}  $*"; }
ok()      { echo -e "${GREEN}[OK]${RESET}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
error()   { echo -e "${RED}[ERROR]${RESET} $*" >&2; }
die()     { error "$*"; exit 1; }

# ── 内部工具函数 ──────────────────────────────────────────────────────────────

# 从 PID 文件读取 PID，若进程存在返回 0，否则返回 1
get_pid() {
    [[ -f "${PID_FILE}" ]] || return 1
    local pid
    pid=$(<"${PID_FILE}")
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null || return 1
    echo "${pid}"
}

# 确保必要目录存在，并创建日志目录到 html 的软链接（供浏览器访问）
prepare_dirs() {
    mkdir -p "${LOG_DIR}" "${HTML_DIR}"
    local link="${HTML_DIR}/logs"
    if [[ ! -L "${link}" ]]; then
        ln -s "${LOG_DIR}" "${link}"
        info "创建软链接: html/logs -> ../logs"
    fi
}

# ── 子命令 ────────────────────────────────────────────────────────────────────

cmd_build() {
    info "开始编译..."
    cd "${SCRIPT_DIR}"
    make 2>&1 | sed 's/^/  /'
    ok "编译完成: ${BIN}"
}

cmd_start() {
    # 检查是否已在运行
    if pid=$(get_pid 2>/dev/null); then
        warn "服务器已在运行 (PID=${pid})，请先执行 stop"
        return 0
    fi

    # 检查二进制文件
    [[ -x "${BIN}" ]] || die "未找到可执行文件 ${BIN}，请先执行: ./server.sh build"

    prepare_dirs

    # 解析额外参数（透传给 simpleserver）
    local extra_args=()
    while [[ $# -gt 0 ]]; do
        extra_args+=("$1")
        shift
    done

    info "启动 simplewebserver..."
    nohup "${BIN}" "${extra_args[@]}" \
        > "${NOHUP_OUT}" 2>&1 &
    local pid=$!
    echo "${pid}" > "${PID_FILE}"

    # 等待短暂确认进程存活
    sleep 0.5
    if kill -0 "${pid}" 2>/dev/null; then
        ok "simplewebserver 已在后台启动 (PID=${pid})"
        # 从参数提取端口（用于显示）
        local port=8081
        for i in "${!extra_args[@]}"; do
            if [[ "${extra_args[$i]}" == "-p" ]]; then
                port="${extra_args[$((i+1))]:-8081}"
            fi
        done
        ok "访问地址:  http://localhost:${port}"
        ok "日志查看:  http://localhost:${port}/logviewer.html"
        info "nohup 输出: ${NOHUP_OUT}"
    else
        rm -f "${PID_FILE}"
        die "启动失败，请查看: ${NOHUP_OUT}"
    fi
}

cmd_stop() {
    local pid
    pid=$(get_pid) || { warn "服务器未在运行"; return 0; }

    info "停止 simplewebserver (PID=${pid})..."
    kill -TERM "${pid}"

    # 等待最多 10 秒
    local waited=0
    while kill -0 "${pid}" 2>/dev/null; do
        sleep 0.5
        (( waited++ ))
        if (( waited >= 20 )); then
            warn "进程未在 10 秒内退出，强制 kill -9"
            kill -9 "${pid}" 2>/dev/null || true
            break
        fi
    done

    rm -f "${PID_FILE}"
    ok "服务器已停止"
}

cmd_restart() {
    info "重启服务器..."
    cmd_stop
    sleep 0.3
    cmd_start "$@"
}

cmd_status() {
    echo -e "${BOLD}── simplewebserver 状态 ────────────────────────${RESET}"
    local pid
    if pid=$(get_pid 2>/dev/null); then
        echo -e "  状态:   ${GREEN}运行中${RESET} (PID=${pid})"
        # 显示内存占用（依赖 /proc，Linux 专用）
        if [[ -f "/proc/${pid}/status" ]]; then
            local vmrss
            vmrss=$(awk '/VmRSS/{print $2, $3}' "/proc/${pid}/status" 2>/dev/null || echo "N/A")
            echo    "  内存:   ${vmrss}"
        fi
        echo    "  启动时: $(ps -p "${pid}" -o lstart= 2>/dev/null || echo 'N/A')"
    else
        echo -e "  状态:   ${RED}未运行${RESET}"
        [[ -f "${PID_FILE}" ]] && { rm -f "${PID_FILE}"; echo "  (已清理过期 PID 文件)"; }
    fi

    echo
    echo -e "${BOLD}── 最近日志 (最新 20 行) ────────────────────${RESET}"
    # 取最新的日志文件
    local latest_log
    latest_log=$(ls -t "${LOG_DIR}"/server_*.log 2>/dev/null | head -1 || true)
    if [[ -n "${latest_log}" ]]; then
        echo "  文件: ${latest_log}"
        tail -n 20 "${latest_log}" | sed 's/^/  /'
    else
        echo "  （暂无日志）"
    fi
    echo -e "${BOLD}────────────────────────────────────────────${RESET}"
}

cmd_usage() {
    cat <<EOF
${BOLD}用法: ./server.sh <命令> [选项]${RESET}

命令:
  build              编译服务器
  start  [选项]      在后台启动服务器
  stop               停止服务器
  restart [选项]     重启服务器
  status             查看运行状态与最近日志

start / restart 选项:
  -p <port>          监听端口       (默认: 8081)
  -t <threads>       工作线程数     (默认: 自动)
  -q <size>          任务队列长度   (默认: 128)
  -l <dir>           日志目录       (默认: logs)

示例:
  ./server.sh build
  ./server.sh start -p 9000 -t 8
  ./server.sh status
  ./server.sh restart -p 9000
  ./server.sh stop
EOF
}

# ── 入口 ──────────────────────────────────────────────────────────────────────
COMMAND="${1:-help}"
shift || true

case "${COMMAND}" in
    build)              cmd_build ;;
    start)              cmd_start "$@" ;;
    stop)               cmd_stop ;;
    restart)            cmd_restart "$@" ;;
    status)             cmd_status ;;
    help|--help|-h)     cmd_usage ;;
    *)                  error "未知命令: ${COMMAND}"; cmd_usage; exit 1 ;;
esac
