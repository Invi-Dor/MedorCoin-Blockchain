#!/bin/bash
set -euo pipefail

# =============================================================================
# MedorCoin Chain Reset Script
# WARNING: This deletes all chain data. Use with extreme caution.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
DATA_DIR="/var/lib/medorcoin"
PID_FILE="/tmp/medorcoin.pid"
BINARY_NAME="medorcoin"
LOG_FILE="$ROOT_DIR/logs/reset.log"
TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
DRY_RUN=false
BACKUP=false
ENVIRONMENT="${MEDORCOIN_ENV:-production}"

# =============================================================================
# Logging
# =============================================================================

log() {
    local level="$1"; shift
    local msg="$*"
    local line="[$TIMESTAMP][$level] $msg"
    echo "$line"
    mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
    echo "$line" >> "$LOG_FILE" 2>/dev/null || true
}

# =============================================================================
# Safety Guards
# =============================================================================

validate_data_dir() {
    # Refuse empty or obviously wrong paths
    if [[ -z "$DATA_DIR" ]]; then
        log ERROR "DATA_DIR is empty. Aborting."
        exit 1
    fi

    if [[ "$DATA_DIR" == "/" ]]; then
        log ERROR "DATA_DIR is '/'. Refusing to operate on root. Aborting."
        exit 1
    fi

    # Must be an absolute path
    if [[ "$DATA_DIR" != /* ]]; then
        log ERROR "DATA_DIR '$DATA_DIR' is not an absolute path. Aborting."
        exit 1
    fi

    # Must have at least 3 path components (e.g. /var/lib/medorcoin)
    local depth
    depth=$(echo "$DATA_DIR" | tr -cd '/' | wc -c)
    if [[ "$depth" -lt 2 ]]; then
        log ERROR "DATA_DIR '$DATA_DIR' is too shallow (depth < 2). Aborting."
        exit 1
    fi

    # Must contain expected app structure (directory signature check)
    local has_signature=false
    for dir in blocks accounts mempool receipts; do
        if [[ -d "$DATA_DIR/$dir" ]]; then
            has_signature=true
            break
        fi
    done

    if [[ "$has_signature" == false ]]; then
        log ERROR "DATA_DIR '$DATA_DIR' does not contain expected MedorCoin subdirectories. Aborting."
        exit 1
    fi

    log INFO "DATA_DIR validated: $DATA_DIR"
}

check_permissions() {
    if [[ ! -w "$DATA_DIR" ]]; then
        log ERROR "No write permission on '$DATA_DIR'. Run as the correct user or with sudo."
        exit 1
    fi
    log INFO "Permission check passed."
}

check_environment() {
    log INFO "Running in environment: $ENVIRONMENT"
    if [[ "$ENVIRONMENT" == "production" ]]; then
        log WARN "You are resetting a PRODUCTION node."
    fi
}

# =============================================================================
# Process Handling
# =============================================================================

stop_node() {
    if [[ ! -f "$PID_FILE" ]]; then
        log INFO "No PID file found. Assuming node is not running."
        return 0
    fi

    local pid
    pid=$(cat "$PID_FILE")

    # Validate PID is a number
    if ! [[ "$pid" =~ ^[0-9]+$ ]]; then
        log WARN "PID file contains invalid value: '$pid'. Removing stale PID file."
        rm -f "$PID_FILE"
        return 0
    fi

    # Check process exists
    if ! kill -0 "$pid" 2>/dev/null; then
        log INFO "PID $pid is not running. Removing stale PID file."
        rm -f "$PID_FILE"
        return 0
    fi

    # Verify the PID belongs to our binary
    local proc_name
    proc_name=$(ps -p "$pid" -o comm= 2>/dev/null || true)
    if [[ "$proc_name" != "$BINARY_NAME" ]]; then
        log ERROR "PID $pid belongs to '$proc_name', not '$BINARY_NAME'. Refusing to stop it. Aborting."
        exit 1
    fi

    log INFO "Stopping node (PID $pid)..."
    if [[ -x "$SCRIPT_DIR/stop_node.sh" ]]; then
        "$SCRIPT_DIR/stop_node.sh" || true
    else
        kill -TERM "$pid" || true
    fi

    # Wait with timeout for the process to die
    local timeout=30
    local elapsed=0
    while kill -0 "$pid" 2>/dev/null; do
        if [[ "$elapsed" -ge "$timeout" ]]; then
            log WARN "Process did not stop after ${timeout}s. Sending SIGKILL..."
            kill -KILL "$pid" 2>/dev/null || true
            sleep 2
            if kill -0 "$pid" 2>/dev/null; then
                log ERROR "Process $pid could not be killed. Aborting reset."
                exit 1
            fi
            break
        fi
        sleep 1
        (( elapsed++ )) || true
    done

    rm -f "$PID_FILE"
    log INFO "Node stopped successfully."
}

# =============================================================================
# Backup
# =============================================================================

backup_chain_data() {
    local backup_dir="$ROOT_DIR/backups/reset_$(date '+%Y%m%d_%H%M%S')"
    log INFO "Creating backup at $backup_dir..."
    mkdir -p "$backup_dir"

    for dir in blocks accounts mempool receipts; do
        if [[ -d "$DATA_DIR/$dir" ]]; then
            cp -r "$DATA_DIR/$dir" "$backup_dir/$dir"
        fi
    done

    log INFO "Backup complete: $backup_dir"
}

# =============================================================================
# Deletion
# =============================================================================

delete_chain_data() {
    local dirs=("blocks" "accounts" "mempool" "receipts")

    for dir in "${dirs[@]}"; do
        local target="$DATA_DIR/$dir"
        if [[ -d "$target" ]]; then
            if [[ "$DRY_RUN" == true ]]; then
                log INFO "[DRY-RUN] Would delete: $target"
            else
                log INFO "Deleting $target..."
                rm -rf "$target"
            fi
        else
            log INFO "Skipping (not found): $target"
        fi
    done
}

clear_logs() {
    local log_files=("node.log" "transactions.log" "errors.log")
    local log_dir="$ROOT_DIR/logs"

    if [[ ! -d "$log_dir" ]]; then
        log WARN "Log directory '$log_dir' does not exist. Skipping log clearing."
        return 0
    fi

    for lf in "${log_files[@]}"; do
        local log_path="$log_dir/$lf"
        if [[ -f "$log_path" ]]; then
            if [[ "$DRY_RUN" == true ]]; then
                log INFO "[DRY-RUN] Would clear: $log_path"
            else
                > "$log_path" && log INFO "Cleared: $log_path"
            fi
        else
            log INFO "Log not found, skipping: $log_path"
        fi
    done
}

# =============================================================================
# Argument Parsing
# =============================================================================

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --dry-run)   DRY_RUN=true ;;
            --backup)    BACKUP=true ;;
            --env)       ENVIRONMENT="$2"; shift ;;
            *)           log WARN "Unknown argument: $1" ;;
        esac
        shift
    done
}

# =============================================================================
# Main
# =============================================================================

main() {
    parse_args "$@"

    log INFO "=== MedorCoin Chain Reset Started ==="
    check_environment
    validate_data_dir
    check_permissions

    if [[ "$DRY_RUN" == true ]]; then
        log INFO "DRY-RUN mode enabled. No changes will be made."
    fi

    echo ""
    echo "  [WARN] This will DELETE all chain data in: $DATA_DIR"
    echo "  [WARN] Environment : $ENVIRONMENT"
    echo "  [WARN] Dry-run     : $DRY_RUN"
    echo "  [WARN] Backup      : $BACKUP"
    echo "  [WARN] This action is IRREVERSIBLE."
    echo ""
    read -r -p "  Type YES to confirm: " CONFIRM

    if [[ "$CONFIRM" != "YES" ]]; then
        log INFO "Reset cancelled by user."
        exit 0
    fi

    stop_node

    if [[ "$BACKUP" == true ]]; then
        backup_chain_data
    fi

    delete_chain_data
    clear_logs

    if [[ "$DRY_RUN" == true ]]; then
        log INFO "=== Dry-run complete. No data was modified. ==="
    else
        log INFO "=== Chain reset complete. Start fresh with: ./scripts/start_node.sh ==="
    fi
}

main "$@"
