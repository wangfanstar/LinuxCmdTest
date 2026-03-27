#!/usr/bin/env bash
# =============================================================================
#  simplewebserver 管理脚本
#  用法: ./simplewebserver.sh {start|stop|restart|status|build} [选项]
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

# 从 PID 文件读取 PID；若进程存在则输出 PID 并返回 0，否则返回 1
get_pid() {
    [[ -f "${PID_FILE}" ]] || return 1
    local pid
    pid=$(<"${PID_FILE}")
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null || return 1
    echo "${pid}"
}

# 检查 PID 对应进程是否属于当前用户；返回 0=属于本用户，1=不属于
owned_by_me() {
    local pid="$1"
    local proc_user
    proc_user=$(ps -p "${pid}" -o user= 2>/dev/null | tr -d '[:space:]') || return 1
    [[ "${proc_user}" == "$(id -un)" ]]
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
    [[ -x "${BIN}" ]] || die "未找到可执行文件 ${BIN}，请先执行: ./simplewebserver.sh build"

    prepare_dirs

    # 解析额外参数（透传给 simpleserver）
    local extra_args=()
    while [[ $# -gt 0 ]]; do
        extra_args+=("$1")
        shift
    done

    info "启动 simplewebserver..."
    nohup "${BIN}" "${extra_args[@]+"${extra_args[@]}"}" \
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

    # 非 root 用户只能终止自己启动的进程
    if [[ "$(id -u)" -ne 0 ]] && ! owned_by_me "${pid}"; then
        die "PID=${pid} 不属于当前用户 $(id -un)，无权终止"
    fi

    info "停止 simplewebserver (PID=${pid})..."

    # 第一步：发送 SIGTERM，等待进程优雅退出（最多 10 秒）
    kill -TERM "${pid}" 2>/dev/null || { warn "发送 SIGTERM 失败，进程可能已退出"; rm -f "${PID_FILE}"; return 0; }

    local waited=0
    while kill -0 "${pid}" 2>/dev/null; do
        sleep 0.5
        (( waited++ ))
        if (( waited >= 20 )); then
            # 第二步：SIGTERM 超时，发送 SIGKILL 强制终止
            warn "进程未在 10 秒内退出，发送 SIGKILL 强制终止..."
            kill -KILL "${pid}" 2>/dev/null || true
            # 再等 2 秒确认彻底退出
            sleep 2
            if kill -0 "${pid}" 2>/dev/null; then
                error "SIGKILL 后进程仍存在，请手动检查 (PID=${pid})"
            fi
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
        echo    "  归属:   $(ps -p "${pid}" -o user= 2>/dev/null || echo 'N/A')"
        echo    "  启动时: $(ps -p "${pid}" -o lstart= 2>/dev/null || echo 'N/A')"
    else
        echo -e "  状态:   ${RED}未运行${RESET}"
        [[ -f "${PID_FILE}" ]] && { rm -f "${PID_FILE}"; echo "  (已清理过期 PID 文件)"; }
    fi

    echo
    echo -e "${BOLD}── 最近日志 (最新 20 行) ────────────────────${RESET}"
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
${BOLD}用法: ./simplewebserver.sh <命令> [选项]${RESET}

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
  ./simplewebserver.sh build
  ./simplewebserver.sh start -p 9000 -t 8
  ./simplewebserver.sh status
  ./simplewebserver.sh restart -p 9000
  ./simplewebserver.sh stop
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
