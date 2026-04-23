#!/usr/bin/env bash
# =============================================================================
#  simplewebserver 管理脚本
#
#  用法: ./simplewebserver.sh <命令> [选项]
#
#  命令:
#    build              仅编译，生成 bin/simplewebserver
#    start  [选项]      后台启动（二进制不存在时自动编译）
#    stop               停止服务器
#    restart [选项]     重新编译并重启服务器
#    status             查看运行状态与最近日志
#    help               显示此帮助
#
#  start / restart 选项（均为可选）：
#    -p <port>      监听端口         (默认: 8881)
#    -t <threads>   工作线程数       (默认: 自动)
#    -q <size>      任务队列长度     (默认: 128)
#    -l <dir>       日志目录         (默认: logs)
#
#  典型流程:
#    首次部署:  ./simplewebserver.sh start          # 自动编译后启动
#    更新代码:  ./simplewebserver.sh restart         # 重编译并重启
#    查看状态:  ./simplewebserver.sh status
#    停止服务:  ./simplewebserver.sh stop
# =============================================================================

set -euo pipefail

# ── 路径配置 ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/bin/simplewebserver"
PID_FILE="${SCRIPT_DIR}/simplewebserver.pid"
LOG_DIR="${SCRIPT_DIR}/logs"
HTML_DIR="${SCRIPT_DIR}/html"
NOHUP_OUT="${LOG_DIR}/nohup.out"

# 默认启用 SQLite（可通过环境变量 SQLITE3=0 临时关闭）
SQLITE3_FLAG="${SQLITE3:-1}"

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

# 确保必要目录存在；/logs/ 路由由服务器直接映射到 logs/ 目录，无需软链接
prepare_dirs() {
    mkdir -p "${LOG_DIR}" "${HTML_DIR}"
    # 清除旧版软链接（如有），避免出现两个 logs 入口
    local link="${HTML_DIR}/logs"
    if [[ -L "${link}" ]]; then
        rm -f "${link}"
        info "已移除旧软链接: html/logs（日志现由 /logs/ 路由直接提供）"
    fi
}

# ── 子命令 ────────────────────────────────────────────────────────────────────

cmd_build() {
    info "开始编译..."
    info "编译选项: SQLITE3=${SQLITE3_FLAG}"
    cd "${SCRIPT_DIR}"

    # 清除旧产物（含旧名称二进制），确保全量重链接到正确路径
    make SQLITE3="${SQLITE3_FLAG}" clean 2>&1 | sed 's/^/  /'

    # 全量编译
    if ! make SQLITE3="${SQLITE3_FLAG}" 2>&1 | sed 's/^/  /'; then
        die "编译失败，请检查上方错误信息"
    fi

    # 验证目标二进制是否在预期位置
    if [[ ! -x "${BIN}" ]]; then
        error "编译完成，但未在预期路径找到可执行文件: ${BIN}"
        error "Makefile 实际输出："
        ls -lh "${SCRIPT_DIR}/bin/" 2>/dev/null | sed 's/^/  /' || true
        die "请确认 Makefile 中 TARGET 变量指向 bin/simplewebserver"
    fi

    ok "编译完成: ${BIN}"
}

cmd_start() {
    # 检查是否已在运行
    if pid=$(get_pid 2>/dev/null); then
        warn "服务器已在运行 (PID=${pid})，请先执行 stop"
        return 0
    fi

    # 二进制不存在时自动编译
    if [[ ! -x "${BIN}" ]]; then
        info "未找到可执行文件，自动编译..."
        cmd_build
    fi

    prepare_dirs

    # 解析额外参数（透传给 simplewebserver）
    local extra_args=()
    while [[ $# -gt 0 ]]; do
        extra_args+=("$1")
        shift
    done

    # 提取并记录端口（供 stop 兜底使用）
    local port=8881
    for i in "${!extra_args[@]}"; do
        if [[ "${extra_args[$i]}" == "-p" ]]; then
            port="${extra_args[$((i+1))]:-8881}"
        fi
    done
    echo "${port}" > "${SCRIPT_DIR}/.port"

    info "启动 simplewebserver..."
    nohup "${BIN}" "${extra_args[@]+"${extra_args[@]}"}" \
        > "${NOHUP_OUT}" 2>&1 &
    local pid=$!
    echo "${pid}" > "${PID_FILE}"

    # 等待短暂确认进程存活
    sleep 0.5
    if kill -0 "${pid}" 2>/dev/null; then
        ok "simplewebserver 已在后台启动 (PID=${pid})"
        ok "访问地址:  http://localhost:${port}"
        ok "日志查看:  http://localhost:${port}/logviewer.html"
        info "nohup 输出: ${NOHUP_OUT}"
    else
        rm -f "${PID_FILE}"
        die "启动失败，请查看: ${NOHUP_OUT}"
    fi
}

# 通过端口号查找占用该端口的 PID（依赖 ss，Linux 通用）
find_pid_by_port() {
    local port="$1"
    ss -tlnp "sport = :${port}" 2>/dev/null \
        | awk 'NR>1 && /users:/ { match($0,/pid=([0-9]+)/,a); if(a[1]) print a[1] }' \
        | head -1
}

# 终止单个 PID，先 SIGTERM 再 SIGKILL
kill_pid() {
    local pid="$1"
    kill -TERM "${pid}" 2>/dev/null || return 0
    local waited=0
    while kill -0 "${pid}" 2>/dev/null; do
        sleep 0.5
        waited=$(( waited + 1 ))
        if (( waited >= 20 )); then
            warn "进程未在 10 秒内退出，发送 SIGKILL 强制终止..."
            kill -KILL "${pid}" 2>/dev/null || true
            sleep 2
            kill -0 "${pid}" 2>/dev/null && error "SIGKILL 后进程仍存在，请手动检查 (PID=${pid})"
            break
        fi
    done
}

cmd_stop() {
    local pid stopped=0

    # ── 优先通过 PID 文件停止 ──────────────────────────────────────
    if pid=$(get_pid 2>/dev/null); then
        if [[ "$(id -u)" -ne 0 ]] && ! owned_by_me "${pid}"; then
            die "PID=${pid} 不属于当前用户 $(id -un)，无权终止"
        fi
        info "停止 simplewebserver (PID=${pid})..."
        kill_pid "${pid}"
        rm -f "${PID_FILE}"
        ok "服务器已停止 (PID=${pid})"
        stopped=1
    else
        [[ -f "${PID_FILE}" ]] && { rm -f "${PID_FILE}"; info "已清理过期 PID 文件"; }
    fi

    # ── 兜底：按端口查找残留进程 ───────────────────────────────────
    # 解析启动端口（从命令行参数或默认值）
    local port=8881
    for i in "${!_START_ARGS[@]:-}"; do
        [[ "${_START_ARGS[$i]:-}" == "-p" ]] && port="${_START_ARGS[$((i+1))]:-8881}"
    done
    # 也从 PID 文件同级的 .port 文件读取（如有）
    [[ -f "${SCRIPT_DIR}/.port" ]] && port=$(<"${SCRIPT_DIR}/.port")

    local port_pid
    port_pid=$(find_pid_by_port "${port}" 2>/dev/null || true)
    if [[ -n "${port_pid}" ]]; then
        if [[ "$(id -u)" -ne 0 ]] && ! owned_by_me "${port_pid}"; then
            warn "端口 ${port} 被 PID=${port_pid}（非本用户）占用，无权终止"
        else
            warn "端口 ${port} 仍被占用 (PID=${port_pid})，强制终止..."
            kill_pid "${port_pid}"
            ok "占用端口 ${port} 的进程已终止 (PID=${port_pid})"
            stopped=1
        fi
    fi

    (( stopped == 0 )) && warn "服务器未在运行"
    return 0
}

cmd_restart() {
    info "重启服务器..."
    cmd_stop
    cmd_build        # 重启时始终重新编译
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
  build              仅编译，生成 bin/simplewebserver
  start  [选项]      后台启动（二进制不存在时自动编译）
  stop               停止服务器
  restart [选项]     重新编译并重启服务器
  status             查看运行状态与最近日志
  help               显示此帮助

start / restart 选项:
  -p <port>          监听端口       (默认: 8881)
  -t <threads>       工作线程数     (默认: 自动，CPU 核数 × 1.5)
  -q <size>          任务队列长度   (默认: 128)
  -l <dir>           日志目录       (默认: logs)

典型流程:
  首次部署   ./simplewebserver.sh start            # 自动编译后启动
  指定端口   ./simplewebserver.sh start -p 9000
  更新代码   ./simplewebserver.sh restart          # 重编译并重启
  查看状态   ./simplewebserver.sh status
  停止服务   ./simplewebserver.sh stop

二进制路径: ${BIN}
日志目录:   ${LOG_DIR}
编译开关:   SQLITE3=${SQLITE3_FLAG}（默认启用，可用 SQLITE3=0 临时关闭）
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
