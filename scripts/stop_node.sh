#!/bin/bash
set -euo pipefail

# =============================================================================
# MedorCoin Node Stop Script
# Production-grade — full signal handling, race mitigation, multi-instance
# =============================================================================

# ── Configuration ─────────────────────────────────────────────────────────────
BINARY_NAME="medorcoin"
NODE_USER="medorcoin"
INSTANCE_ID="${MEDORCOIN_INSTANCE:-default}"
BASE_DIR="/var/lib/medorcoin"
DATA_DIR="${MEDORCOIN_DATA_DIR:-$BASE_DIR/$INSTANCE_ID}"
PID_FILE="${MEDORCOIN_PID_FILE:-/tmp/medorcoin_${INSTANCE_ID}.pid}"
LOG_DIR="${MEDORCOIN_LOG_DIR:-/var/log/medorcoin/$INSTANCE_ID}"
LOG_FILE="$LOG_DIR/stop_node.log"
SHUTDOWN_MARKER="$DATA_DIR/.last_clean_shutdown"
GRACEFUL_TIMEOUT="${MEDORCOIN_STOP_TIMEOUT:-30}"
HOOK_FAIL_FATAL="${MEDORCOIN_HOOK_FAIL_FATAL:-false}"
HOOK_RETRY_COUNT="${MEDORCOIN_HOOK_RETRIES:-2}"
SKIP_USER_CHECK=false
STRICT_LOG=false

# Lock files — override via MEDORCOIN_LOCK_FILES (colon-separated)
if [[ -n "${MEDORCOIN_LOCK_FILES:-}" ]]; then
    IFS=':' read -ra LOCK_FILES <<< "$MEDORCOIN_LOCK_FILES"
else
    LOCK_FILES=(
        "$DATA_DIR/node.lock"
        "$DATA_DIR/.lock"
        "$DATA_DIR/LOCK"
    )
fi

# Exit codes
EXIT_CODE_OK=0
EXIT_CODE_FAIL=1
EXIT_CODE_FORCE=2
EXIT_CODE_PERMISSION=3
EXIT_CODE_HOOK_FAIL=4

# ── Cleanup state — tracked for trap ─────────────────────────────────────────
_CLEANUP_PID=""
_CLEANUP_DONE=false

# =============================================================================
# Logging
# =============================================================================

_ensure_log_dir() {
    if [[ -d "$LOG_DIR" ]]; then return 0; fi
    mkdir -p "$LOG_DIR" 2>/dev/null || return 1
    touch "$LOG_FILE" 2>/dev/null  || return 1
    return 0
}

log() {
    local level="$1"; shift
    local msg="$*"
    local ts
    ts=$(date '+%Y-%m-%d %H:%M:%S')
    local line="[$ts][$level][instance:$INSTANCE_ID] $msg"
    echo "$line"

    if _ensure_log_dir; then
        if ! echo "$line" >> "$LOG_FILE" 2>/dev/null; then
            # Log write failed
            if [[ "$STRICT_LOG" == true ]]; then
                # In strict mode: warn to stderr but DO NOT abort shutdown.
                # Aborting shutdown because of a log failure is worse than
                # losing the log entry — the node must stop regardless.
                echo "[WARN] Log write failed — continuing shutdown (strict log)" >&2
            fi
        fi
    else
        if [[ "$STRICT_LOG" == true ]]; then
            echo "[WARN] Log dir unavailable — continuing shutdown (strict log)" >&2
        fi
    fi
}

# =============================================================================
# Signal trap — cleanup AND stop the node on script interruption
# =============================================================================

_trap_cleanup() {
    if [[ "$_CLEANUP_DONE" == true ]]; then return; fi
    _CLEANUP_DONE=true

    log WARN "Script interrupted. Running emergency cleanup..."

    if [[ -n "$_CLEANUP_PID" ]]; then
        # If the node is still alive, attempt to stop it
        if kill -0 "$_CLEANUP_PID" 2>/dev/null; then
            log WARN "Node PID $_CLEANUP_PID still running during interrupt — sending SIGTERM..."
            kill -SIGTERM "$_CLEANUP_PID" 2>/dev/null || true

            # Wait briefly for graceful exit
            local waited=0
            while kill -0 "$_CLEANUP_PID" 2>/dev/null && [[ "$waited" -lt 10 ]]; do
                sleep 1
                (( waited++ )) || true
            done

            # Escalate if still alive
            if kill -0 "$_CLEANUP_PID" 2>/dev/null; then
                log WARN "Node did not stop — sending SIGKILL to PID $_CLEANUP_PID..."
                kill -SIGKILL "$_CLEANUP_PID" 2>/dev/null || true
                sleep 1
            fi
        fi

        # Only remove PID file if node is now confirmed dead
        if ! kill -0 "$_CLEANUP_PID" 2>/dev/null; then
            rm -f "$PID_FILE" 2>/dev/null || true
            log INFO "PID file removed during interrupt cleanup."
        else
            log ERROR "Node PID $_CLEANUP_PID could not be stopped during interrupt. PID file left intact."
        fi
    fi

    _cleanup_lockfiles_silent
    sync 2>/dev/null || true
    log WARN "Emergency cleanup complete. Script was interrupted."
    exit $EXIT_CODE_FAIL
}

_cleanup_lockfiles_silent() {
    for lf in "${LOCK_FILES[@]}"; do
        [[ -f "$lf" ]] && rm -f "$lf" 2>/dev/null || true
    done
}

trap '_trap_cleanup' SIGINT SIGTERM SIGHUP

# =============================================================================
# Permission / user check
# =============================================================================

check_permissions() {
    local current_user
    current_user=$(id -un)
    if [[ "$current_user" != "root" && "$current_user" != "$NODE_USER" ]]; then
        log ERROR "Must be run as '$NODE_USER' or root. Current user: '$current_user'."
        exit $EXIT_CODE_PERMISSION
    fi
    log INFO "Permission check passed (user: $current_user)."
}

# =============================================================================
# PID validation
# =============================================================================

read_and_validate_pid() {
    if [[ ! -f "$PID_FILE" ]]; then
        log INFO "No PID file at '$PID_FILE'. Node may not be running."
        exit $EXIT_CODE_OK
    fi

    local pid
    pid=$(cat "$PID_FILE" 2>/dev/null || true)

    if [[ -z "$pid" || ! "$pid" =~ ^[0-9]+$ ]]; then
        log WARN "PID file contains invalid value: '${pid:-<empty>}'. Removing stale file."
        rm -f "$PID_FILE"
        exit $EXIT_CODE_OK
    fi

    echo "$pid"
}

# =============================================================================
# Process identity — race-safe, portable
# =============================================================================

verify_process_identity() {
    local pid="$1"
    local proc_name=""

    # Linux-fast path via /proc; falls back to ps on macOS/BSD
    if [[ -r "/proc/$pid/comm" ]]; then
        proc_name=$(cat "/proc/$pid/comm" 2>/dev/null || true)
    else
        proc_name=$(ps -p "$pid" -o comm= 2>/dev/null || true)
    fi

    if [[ -z "$proc_name" ]]; then
        log INFO "PID $pid is not running. Removing stale PID file."
        rm -f "$PID_FILE"
        exit $EXIT_CODE_OK
    fi

    if [[ "$proc_name" != "$BINARY_NAME" ]]; then
        log ERROR "PID $pid belongs to '$proc_name', not '$BINARY_NAME'. Refusing to act."
        exit $EXIT_CODE_FAIL
    fi

    # Second kill -0 to close the read→signal race window
    if ! kill -0 "$pid" 2>/dev/null; then
        log INFO "PID $pid exited between identity check and signal. Cleaning up."
        rm -f "$PID_FILE"
        exit $EXIT_CODE_OK
    fi

    log INFO "Process identity confirmed: PID $pid is '$BINARY_NAME'."
}

# =============================================================================
# Application-level flush
# =============================================================================

application_flush() {
    local pid="$1"

    if ! kill -0 "$pid" 2>/dev/null; then return; fi

    # Attempt SIGUSR1 as application flush signal.
    # Nodes that don't handle SIGUSR1 will ignore it (default disposition).
    # Nodes that do handle it can flush DB, mempool, wallet state, etc.
    log INFO "Sending SIGUSR1 (application flush signal) to PID $pid..."
    if kill -SIGUSR1 "$pid" 2>/dev/null; then
        log INFO "SIGUSR1 delivered. Waiting 2s for application-level flush..."
        sleep 2
    else
        log WARN "SIGUSR1 delivery failed (process may have already exited). Continuing."
    fi

    # Kernel buffer flush — best effort regardless of SIGUSR1 outcome
    log INFO "Flushing filesystem buffers (sync)..."
    sync 2>/dev/null || true
}

# =============================================================================
# Graceful shutdown
# =============================================================================

send_sigterm() {
    local pid="$1"
    log INFO "Sending SIGTERM to PID $pid..."
    if ! kill -SIGTERM "$pid" 2>/dev/null; then
        log ERROR "Failed to send SIGTERM to PID $pid."
        exit $EXIT_CODE_FAIL
    fi
}

wait_for_exit() {
    local pid="$1"
    local timeout="$2"
    local elapsed=0

    log INFO "Waiting up to ${timeout}s for graceful shutdown..."
    while kill -0 "$pid" 2>/dev/null; do
        if [[ "$elapsed" -ge "$timeout" ]]; then
            log WARN "Process did not stop after ${timeout}s."
            return 1
        fi
        sleep 1
        (( elapsed++ )) || true
    done
    return 0
}

# =============================================================================
# Force kill
# =============================================================================

force_kill() {
    local pid="$1"
    log WARN "Escalating to SIGKILL for PID $pid..."
    if ! kill -SIGKILL "$pid" 2>/dev/null; then
        log ERROR "Failed to send SIGKILL to PID $pid. Manual intervention required."
        exit $EXIT_CODE_FAIL
    fi
    sleep 2
    if kill -0 "$pid" 2>/dev/null; then
        log ERROR "PID $pid still alive after SIGKILL. Manual intervention required."
        exit $EXIT_CODE_FAIL
    fi
    log WARN "PID $pid forcefully killed."
}

# =============================================================================
# Lock file cleanup — with networked FS awareness
# =============================================================================

cleanup_lockfiles() {
    local cleaned=0
    for lf in "${LOCK_FILES[@]}"; do
        if [[ -f "$lf" ]]; then
            log INFO "Removing lock file: $lf"
            # Use a subshell with timeout to guard against hung networked FS
            if ( timeout 5s rm -f "$lf" ) 2>/dev/null; then
                (( cleaned++ )) || true
            else
                log WARN "Failed to remove lock file (possible networked FS hang): $lf"
            fi
        fi
    done
    [[ "$cleaned" -eq 0 ]] && log INFO "No lock files found."
}

# =============================================================================
# Shutdown marker
# =============================================================================

write_shutdown_marker() {
    local reason="$1"
    local ts
    ts=$(date '+%Y-%m-%d %H:%M:%S')

    if [[ ! -d "$DATA_DIR" ]]; then
        log WARN "DATA_DIR '$DATA_DIR' not found. Skipping shutdown marker."
        return 0
    fi

    {
        echo "shutdown_time=$ts"
        echo "shutdown_reason=$reason"
        echo "instance=$INSTANCE_ID"
        echo "pid_file=$PID_FILE"
    } > "$SHUTDOWN_MARKER" 2>/dev/null || log WARN "Failed to write shutdown marker."

    log INFO "Shutdown marker written: $SHUTDOWN_MARKER"
}

# =============================================================================
# Monitoring hooks — retry + failure detail
# =============================================================================

emit_monitoring_signal() {
    local status="$1"
    local hook_failed=false

    # systemd notify
    if command -v systemd-notify &>/dev/null; then
        systemd-notify --status="MedorCoin[$INSTANCE_ID] stopped ($status)" 2>/dev/null || true
    fi

    # External hook with retry
    local hook="/etc/medorcoin/hooks/on_stop.sh"
    if [[ -x "$hook" ]]; then
        local attempt=0
        local hook_ok=false
        while [[ "$attempt" -le "$HOOK_RETRY_COUNT" ]]; do
            log INFO "Running monitoring hook (attempt $((attempt+1))/$((HOOK_RETRY_COUNT+1))): $hook $status $INSTANCE_ID"
            local hook_output
            if hook_output=$("$hook" "$status" "$INSTANCE_ID" 2>&1); then
                log INFO "Hook succeeded."
                hook_ok=true
                break
            else
                log WARN "Hook failed (attempt $((attempt+1))). Output: $hook_output"
                (( attempt++ )) || true
                [[ "$attempt" -le "$HOOK_RETRY_COUNT" ]] && sleep 2
            fi
        done
        if [[ "$hook_ok" == false ]]; then
            hook_failed=true
            log ERROR "Hook '$hook' failed after $((HOOK_RETRY_COUNT+1)) attempts."
        fi
    fi

    # Machine-readable status for orchestrators
    echo "$status" > "/tmp/medorcoin_${INSTANCE_ID}_last_stop_status" 2>/dev/null || true

    if [[ "$hook_failed" == true && "$HOOK_FAIL_FATAL" == true ]]; then
        log ERROR "Hook failure is fatal (MEDORCOIN_HOOK_FAIL_FATAL=true)."
        exit $EXIT_CODE_HOOK_FAIL
    fi
}

# =============================================================================
# Argument parsing
# =============================================================================

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --timeout)         GRACEFUL_TIMEOUT="$2";
                               [[ "$GRACEFUL_TIMEOUT" =~ ^[0-9]+$ ]] || { echo "[ERROR] --timeout must be a positive integer." >&2; exit $EXIT_CODE_FAIL; }
                               shift ;;
            --pid-file)        PID_FILE="$2";       shift ;;
            --data-dir)        DATA_DIR="$2";       shift ;;
            --log-dir)         LOG_DIR="$2"; LOG_FILE="$LOG_DIR/stop_node.log"; shift ;;
            --instance)        INSTANCE_ID="$2";    shift ;;
            --lock-files)      IFS=':' read -ra LOCK_FILES <<< "$2"; shift ;;
            --hook-retries)    HOOK_RETRY_COUNT="$2"; shift ;;
            --hook-fail-fatal) HOOK_FAIL_FATAL=true ;;
            --strict-log)      STRICT_LOG=true ;;
            --skip-user-check) SKIP_USER_CHECK=true ;;
            *) log WARN "Unknown argument: $1" ;;
        esac
        shift
    done
}

# =============================================================================
# Main
# =============================================================================

main() {
    parse_args "$@"

    log INFO "=== MedorCoin Stop Script Started (instance: $INSTANCE_ID) ==="

    [[ "$SKIP_USER_CHECK" == false ]] && check_permissions

    local pid
    pid=$(read_and_validate_pid)
    _CLEANUP_PID="$pid"

    verify_process_identity "$pid"

    log INFO "Stopping MedorCoin node (PID $pid, instance: $INSTANCE_ID)..."

    application_flush "$pid"
    send_sigterm "$pid"

    if wait_for_exit "$pid" "$GRACEFUL_TIMEOUT"; then
        sync 2>/dev/null || true
        cleanup_lockfiles
        write_shutdown_marker "graceful"
        rm -f "$PID_FILE"
        _CLEANUP_DONE=true
        emit_monitoring_signal "clean"
        log INFO "=== Node stopped gracefully (instance: $INSTANCE_ID) ==="
        exit $EXIT_CODE_OK
    fi

    force_kill "$pid"
    sync 2>/dev/null || true
    cleanup_lockfiles
    write_shutdown_marker "forced"
    rm -f "$PID_FILE"
    _CLEANUP_DONE=true
    emit_monitoring_signal "forced"
    log WARN "=== Node forcefully stopped (instance: $INSTANCE_ID) ==="
    exit $EXIT_CODE_FORCE
}

main "$@"
