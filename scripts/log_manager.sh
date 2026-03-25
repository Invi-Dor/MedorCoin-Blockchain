#!/bin/bash
set -euo pipefail

# =============================================================================
# MedorCoin Log Manager
# Production-grade — rotation, retention, structured logging, multi-instance,
# centralized shipping, alerting hooks, permissions, multi-region support,
# audit log, dynamic log level, syslog/journald integration
# =============================================================================

# ── Instance / region identity ─────────────────────────────────────────────────
INSTANCE_ID="${MEDORCOIN_INSTANCE:-default}"
REGION="${MEDORCOIN_REGION:-local}"
NODE_ID="${MEDORCOIN_NODE_ID:-$(hostname)}"

# ── Paths ──────────────────────────────────────────────────────────────────────
LOG_BASE="${MEDORCOIN_LOG_BASE:-/var/log/medorcoin}"
LOG_DIR="$LOG_BASE/$INSTANCE_ID"
LOG_FILE="$LOG_DIR/node.log"
AUDIT_LOG="$LOG_DIR/audit.log"
ERROR_LOG="$LOG_DIR/error.log"
ARCHIVE_DIR="$LOG_DIR/archive"
LEVEL_FILE="$LOG_DIR/.log_level"          # dynamic level control
MANAGER_LOG="$LOG_DIR/log_manager.log"    # self-log for the manager

# ── Rotation / retention ───────────────────────────────────────────────────────
MAX_LOG_SIZE_MB="${MEDORCOIN_LOG_MAX_MB:-100}"
MAX_ARCHIVES="${MEDORCOIN_LOG_MAX_ARCHIVES:-14}"
ROTATE_ON_START="${MEDORCOIN_ROTATE_ON_START:-false}"

# ── Log format ─────────────────────────────────────────────────────────────────
LOG_FORMAT="${MEDORCOIN_LOG_FORMAT:-plain}"   # plain | json

# ── Log level ─────────────────────────────────────────────────────────────────
# Levels: DEBUG=0 INFO=1 WARN=2 ERROR=3 FATAL=4
CURRENT_LEVEL="${MEDORCOIN_LOG_LEVEL:-INFO}"

# ── Ownership / permissions ────────────────────────────────────────────────────
LOG_OWNER="${MEDORCOIN_LOG_OWNER:-medorcoin}"
LOG_GROUP="${MEDORCOIN_LOG_GROUP:-medorcoin}"
LOG_FILE_MODE="640"
LOG_DIR_MODE="750"

# ── Centralized shipping ───────────────────────────────────────────────────────
SHIP_TO_SYSLOG="${MEDORCOIN_SYSLOG:-false}"
SHIP_TO_JOURNALD="${MEDORCOIN_JOURNALD:-false}"
SHIP_TO_LOKI="${MEDORCOIN_LOKI_URL:-}"        # e.g. http://loki:3100/loki/api/v1/push
SHIP_TO_CLOUDWATCH="${MEDORCOIN_CLOUDWATCH:-false}"
SHIP_LOGGROUP="${MEDORCOIN_CW_LOG_GROUP:-/medorcoin/nodes}"

# ── Alerting ───────────────────────────────────────────────────────────────────
ALERT_WEBHOOK="${MEDORCOIN_ALERT_WEBHOOK:-}"  # Slack/Teams/PagerDuty URL
ALERT_EMAIL="${MEDORCOIN_ALERT_EMAIL:-}"
ALERT_ON_LEVELS="ERROR FATAL"
ALERT_RETRY_COUNT="${MEDORCOIN_ALERT_RETRIES:-2}"

# ── Audit logging ──────────────────────────────────────────────────────────────
AUDIT_ENABLED="${MEDORCOIN_AUDIT:-true}"
AUDIT_HMAC_KEY="${MEDORCOIN_AUDIT_KEY:-}"     # if set, each audit line is HMAC-signed

# ── Exit codes ─────────────────────────────────────────────────────────────────
EXIT_OK=0
EXIT_FAIL=1
EXIT_PERMISSION=3

# =============================================================================
# Internal / manager self-logging (always plain, never recursive)
# =============================================================================

_mlog() {
    local level="$1"; shift
    local ts
    ts=$(date '+%Y-%m-%d %H:%M:%S')
    local line="[$ts][$level][log_manager][$INSTANCE_ID] $*"
    echo "$line"
    echo "$line" >> "$MANAGER_LOG" 2>/dev/null || true
}

# =============================================================================
# Level resolution
# =============================================================================

declare -A _LEVEL_MAP=([DEBUG]=0 [INFO]=1 [WARN]=2 [ERROR]=3 [FATAL]=4)

_resolve_level() {
    # Dynamic override: if LEVEL_FILE exists, it takes precedence
    if [[ -f "$LEVEL_FILE" ]]; then
        local dyn
        dyn=$(cat "$LEVEL_FILE" 2>/dev/null | tr -d '[:space:]' | tr '[:lower:]' '[:upper:]')
        if [[ -n "${_LEVEL_MAP[$dyn]+_}" ]]; then
            echo "$dyn"; return
        fi
    fi
    echo "${CURRENT_LEVEL^^}"
}

_level_passes() {
    local msg_level="$1"
    local cur
    cur=$(_resolve_level)
    local msg_n="${_LEVEL_MAP[$msg_level]:-1}"
    local cur_n="${_LEVEL_MAP[$cur]:-1}"
    [[ "$msg_n" -ge "$cur_n" ]]
}

# =============================================================================
# Permissions setup
# =============================================================================

setup_permissions() {
    _mlog INFO "Setting up log directory permissions..."

    mkdir -p "$LOG_DIR" "$ARCHIVE_DIR"
    chmod "$LOG_DIR_MODE" "$LOG_DIR" "$ARCHIVE_DIR"

    local current_user
    current_user=$(id -un)

    # Only attempt chown if root
    if [[ "$current_user" == "root" ]]; then
        chown "$LOG_OWNER:$LOG_GROUP" "$LOG_DIR" "$ARCHIVE_DIR"
    fi

    # Ensure log files exist with correct permissions
    for f in "$LOG_FILE" "$AUDIT_LOG" "$ERROR_LOG" "$MANAGER_LOG"; do
        touch "$f" 2>/dev/null || true
        chmod "$LOG_FILE_MODE" "$f" 2>/dev/null || true
        if [[ "$current_user" == "root" ]]; then
            chown "$LOG_OWNER:$LOG_GROUP" "$f" 2>/dev/null || true
        fi
    done

    _mlog INFO "Permissions applied (mode: $LOG_FILE_MODE, owner: $LOG_OWNER:$LOG_GROUP)."
}

# =============================================================================
# Log rotation
# =============================================================================

_file_size_mb() {
    local f="$1"
    if [[ ! -f "$f" ]]; then echo 0; return; fi
    local sz
    sz=$(wc -c < "$f" 2>/dev/null || echo 0)
    echo $(( sz / 1024 / 1024 ))
}

rotate_if_needed() {
    local f="${1:-$LOG_FILE}"
    local sz
    sz=$(_file_size_mb "$f")

    if [[ "$sz" -lt "$MAX_LOG_SIZE_MB" && "$ROTATE_ON_START" == false ]]; then
        return 0
    fi

    _mlog INFO "Rotating $f (size: ${sz}MB, threshold: ${MAX_LOG_SIZE_MB}MB)..."

    local ts
    ts=$(date '+%Y%m%d_%H%M%S')
    local archive="$ARCHIVE_DIR/$(basename "$f").$ts.gz"

    gzip -c "$f" > "$archive" 2>/dev/null && {
        > "$f"
        chmod "$LOG_FILE_MODE" "$f" 2>/dev/null || true
        _mlog INFO "Rotated to $archive"
    } || _mlog WARN "Rotation failed for $f"

    _prune_archives
}

_prune_archives() {
    local count
    count=$(ls -1 "$ARCHIVE_DIR"/*.gz 2>/dev/null | wc -l || echo 0)
    if [[ "$count" -le "$MAX_ARCHIVES" ]]; then return; fi

    local excess=$(( count - MAX_ARCHIVES ))
    _mlog INFO "Pruning $excess old archive(s)..."
    ls -1t "$ARCHIVE_DIR"/*.gz 2>/dev/null | tail -n "$excess" | xargs rm -f 2>/dev/null || true
}

# =============================================================================
# Structured log entry formatting
# =============================================================================

_format_plain() {
    local level="$1" module="$2" msg="$3" ts="$4"
    echo "[$ts] [$level] [$INSTANCE_ID] [$REGION] [$NODE_ID] [$module] $msg"
}

_format_json() {
    local level="$1" module="$2" msg="$3" ts="$4"
    # Escape double quotes in msg
    msg="${msg//\"/\\\"}"
    printf '{"timestamp":"%s","level":"%s","instance":"%s","region":"%s","node_id":"%s","module":"%s","message":"%s"}\n' \
        "$ts" "$level" "$INSTANCE_ID" "$REGION" "$NODE_ID" "$module" "$msg"
}

_format_entry() {
    local level="$1" module="$2" msg="$3"
    local ts
    ts=$(date '+%Y-%m-%dT%H:%M:%SZ')

    if [[ "$LOG_FORMAT" == "json" ]]; then
        _format_json "$level" "$module" "$msg" "$ts"
    else
        _format_plain "$level" "$module" "$msg" "$ts"
    fi
}

# =============================================================================
# Core write function
# =============================================================================

write_log() {
    local level="${1^^}"
    local module="${2:-core}"
    local msg="$3"

    # Level filter
    _level_passes "$level" || return 0

    local entry
    entry=$(_format_entry "$level" "$module" "$msg")

    # Rotate before writing if needed
    rotate_if_needed "$LOG_FILE"

    # Write to main log
    echo "$entry" >> "$LOG_FILE" 2>/dev/null || true

    # Mirror errors to error log
    if [[ "$level" == "ERROR" || "$level" == "FATAL" ]]; then
        echo "$entry" >> "$ERROR_LOG" 2>/dev/null || true
    fi

    # Audit log
    if [[ "$AUDIT_ENABLED" == true ]]; then
        _write_audit "$entry"
    fi

    # Centralized shipping
    _ship "$level" "$entry" "$msg"

    # Alerts
    _maybe_alert "$level" "$entry" "$msg"
}

# =============================================================================
# Audit logging — optional HMAC signing
# =============================================================================

_write_audit() {
    local entry="$1"
    local line="$entry"

    if [[ -n "$AUDIT_HMAC_KEY" ]] && command -v openssl &>/dev/null; then
        local sig
        sig=$(echo -n "$entry" | openssl dgst -sha256 -hmac "$AUDIT_HMAC_KEY" -hex 2>/dev/null \
              | awk '{print $2}')
        line="$entry HMAC=$sig"
    fi

    echo "$line" >> "$AUDIT_LOG" 2>/dev/null || true
}

# =============================================================================
# Centralized shipping
# =============================================================================

_ship() {
    local level="$1"
    local entry="$2"
    local msg="$3"

    # syslog
    if [[ "$SHIP_TO_SYSLOG" == true ]] && command -v logger &>/dev/null; then
        local priority="user.info"
        [[ "$level" == "WARN"  ]] && priority="user.warning"
        [[ "$level" == "ERROR" ]] && priority="user.err"
        [[ "$level" == "FATAL" ]] && priority="user.crit"
        logger -t "medorcoin[$INSTANCE_ID]" -p "$priority" "$msg" 2>/dev/null || true
    fi

    # journald
    if [[ "$SHIP_TO_JOURNALD" == true ]] && command -v systemd-cat &>/dev/null; then
        local priority="info"
        [[ "$level" == "WARN"  ]] && priority="warning"
        [[ "$level" == "ERROR" ]] && priority="err"
        [[ "$level" == "FATAL" ]] && priority="crit"
        echo "$msg" | systemd-cat -t "medorcoin-$INSTANCE_ID" -p "$priority" 2>/dev/null || true
    fi

    # Grafana Loki (JSON push)
    if [[ -n "$SHIP_TO_LOKI" ]] && command -v curl &>/dev/null; then
        local ts_ns
        ts_ns=$(date '+%s%N' 2>/dev/null || echo "0")
        local payload
        payload=$(printf '{"streams":[{"stream":{"instance":"%s","region":"%s","node":"%s","level":"%s"},"values":[["%s","%s"]]}]}' \
            "$INSTANCE_ID" "$REGION" "$NODE_ID" "$level" "$ts_ns" "${msg//\"/\\\"}")
        curl -sf -X POST "$SHIP_TO_LOKI" \
             -H "Content-Type: application/json" \
             -d "$payload" \
             --max-time 3 2>/dev/null || true
    fi

    # AWS CloudWatch (requires aws-cli configured)
    if [[ "$SHIP_TO_CLOUDWATCH" == true ]] && command -v aws &>/dev/null; then
        local ts_ms
        ts_ms=$(date '+%s%3N' 2>/dev/null || echo "0")
        aws logs put-log-events \
            --log-group-name  "$SHIP_LOGGROUP" \
            --log-stream-name "$INSTANCE_ID-$NODE_ID" \
            --log-events      "timestamp=$ts_ms,message=${entry}" \
            --region "${AWS_DEFAULT_REGION:-us-east-1}" \
            2>/dev/null || true
    fi
}

# =============================================================================
# Alerting — webhook + email with retry
# =============================================================================

_maybe_alert() {
    local level="$1"
    local entry="$2"
    local msg="$3"

    # Check if this level triggers alerts
    local should_alert=false
    for al in $ALERT_ON_LEVELS; do
        [[ "$level" == "$al" ]] && should_alert=true && break
    done
    [[ "$should_alert" == false ]] && return 0

    # Webhook (Slack / Teams / PagerDuty)
    if [[ -n "$ALERT_WEBHOOK" ]] && command -v curl &>/dev/null; then
        local payload
        payload=$(printf '{"text":"[MedorCoin][%s][%s][%s] %s: %s"}' \
            "$INSTANCE_ID" "$REGION" "$NODE_ID" "$level" "${msg//\"/\\\"}")
        local attempt=0
        while [[ "$attempt" -le "$ALERT_RETRY_COUNT" ]]; do
            if curl -sf -X POST "$ALERT_WEBHOOK" \
                    -H "Content-Type: application/json" \
                    -d "$payload" --max-time 5 2>/dev/null; then
                break
            fi
            (( attempt++ )) || true
            [[ "$attempt" -le "$ALERT_RETRY_COUNT" ]] && sleep 2
        done
    fi

    # Email
    if [[ -n "$ALERT_EMAIL" ]] && command -v mail &>/dev/null; then
        echo "$entry" | mail -s "[MedorCoin][$INSTANCE_ID][$level] Node Alert" \
            "$ALERT_EMAIL" 2>/dev/null || true
    fi
}

# =============================================================================
# Dynamic log level control
# =============================================================================

set_log_level() {
    local new_level="${1^^}"
    if [[ -z "${_LEVEL_MAP[$new_level]+_}" ]]; then
        _mlog ERROR "Invalid log level: $new_level. Valid: DEBUG INFO WARN ERROR FATAL"
        exit $EXIT_FAIL
    fi
    echo "$new_level" > "$LEVEL_FILE"
    _mlog INFO "Log level set to $new_level (written to $LEVEL_FILE)."
}

get_log_level() {
    local cur
    cur=$(_resolve_level)
    echo "Current log level: $cur (instance: $INSTANCE_ID)"
}

# =============================================================================
# Argument parsing + commands
# =============================================================================

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] COMMAND

Commands:
  setup                  Create dirs, apply permissions
  rotate                 Force rotate main log now
  rotate-all             Force rotate all logs (node, error, audit)
  write LEVEL MODULE MSG Write a log entry (LEVEL: DEBUG|INFO|WARN|ERROR|FATAL)
  set-level LEVEL        Dynamically change log level (no restart needed)
  get-level              Print current effective log level
  prune                  Prune old archives beyond retention limit
  status                 Print log file sizes and archive count

Options:
  --instance ID          Instance ID (default: \$MEDORCOIN_INSTANCE or 'default')
  --region REGION        Region tag (default: \$MEDORCOIN_REGION or 'local')
  --format plain|json    Log format (default: plain)
  --level LEVEL          Minimum log level (default: INFO)
  --max-size MB          Max log size before rotation (default: 100)
  --max-archives N       Max archived logs to keep (default: 14)
  --rotate-on-start      Force rotate when script starts
  --syslog               Ship to syslog
  --journald             Ship to journald
  --loki URL             Ship to Grafana Loki
  --cloudwatch           Ship to AWS CloudWatch
  --webhook URL          Alert webhook (Slack/Teams/PagerDuty)
  --email ADDRESS        Alert email address
  --audit-key KEY        HMAC key for tamper-evident audit log
  --strict               Fail on permission or setup errors
  --no-audit             Disable audit logging
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --instance)      INSTANCE_ID="$2";
                             LOG_DIR="$LOG_BASE/$INSTANCE_ID"
                             LOG_FILE="$LOG_DIR/node.log"
                             AUDIT_LOG="$LOG_DIR/audit.log"
                             ERROR_LOG="$LOG_DIR/error.log"
                             ARCHIVE_DIR="$LOG_DIR/archive"
                             MANAGER_LOG="$LOG_DIR/log_manager.log"
                             LEVEL_FILE="$LOG_DIR/.log_level"
                             shift ;;
            --region)        REGION="$2";          shift ;;
            --format)        LOG_FORMAT="$2";      shift ;;
            --level)         CURRENT_LEVEL="${2^^}"; shift ;;
            --max-size)      MAX_LOG_SIZE_MB="$2"; shift ;;
            --max-archives)  MAX_ARCHIVES="$2";    shift ;;
            --rotate-on-start) ROTATE_ON_START=true ;;
            --syslog)        SHIP_TO_SYSLOG=true ;;
            --journald)      SHIP_TO_JOURNALD=true ;;
            --loki)          SHIP_TO_LOKI="$2";    shift ;;
            --cloudwatch)    SHIP_TO_CLOUDWATCH=true ;;
            --webhook)       ALERT_WEBHOOK="$2";   shift ;;
            --email)         ALERT_EMAIL="$2";     shift ;;
            --audit-key)     AUDIT_HMAC_KEY="$2";  shift ;;
            --no-audit)      AUDIT_ENABLED=false ;;
            --strict)        set -e ;;
            -h|--help)       usage; exit $EXIT_OK ;;
            *)               break ;;
        esac
        shift
    done
    COMMAND="${1:-help}"
    shift || true
    CMD_ARGS=("$@")
}

# =============================================================================
# Status command
# =============================================================================

print_status() {
    echo "=== MedorCoin Log Manager Status ==="
    echo "  Instance    : $INSTANCE_ID"
    echo "  Region      : $REGION"
    echo "  Node ID     : $NODE_ID"
    echo "  Log format  : $LOG_FORMAT"
    echo "  Log level   : $(_resolve_level)"
    echo "  Log dir     : $LOG_DIR"
    echo ""
    for f in "$LOG_FILE" "$ERROR_LOG" "$AUDIT_LOG" "$MANAGER_LOG"; do
        local sz
        sz=$(_file_size_mb "$f")
        printf "  %-30s %s MB\n" "$(basename "$f")" "$sz"
    done
    local archives
    archives=$(ls -1 "$ARCHIVE_DIR"/*.gz 2>/dev/null | wc -l || echo 0)
    echo ""
    echo "  Archives    : $archives / $MAX_ARCHIVES max"
    echo "  Syslog      : $SHIP_TO_SYSLOG"
    echo "  Journald    : $SHIP_TO_JOURNALD"
    echo "  Loki        : ${SHIP_TO_LOKI:-disabled}"
    echo "  CloudWatch  : $SHIP_TO_CLOUDWATCH"
    echo "  Alert hook  : ${ALERT_WEBHOOK:-disabled}"
    echo "  Alert email : ${ALERT_EMAIL:-disabled}"
    echo "  Audit HMAC  : $( [[ -n "$AUDIT_HMAC_KEY" ]] && echo enabled || echo disabled )"
}

# =============================================================================
# Main
# =============================================================================

CMD_ARGS=()

main() {
    parse_args "$@"

    case "$COMMAND" in
        setup)
            setup_permissions ;;
        rotate)
            ROTATE_ON_START=true rotate_if_needed "$LOG_FILE" ;;
        rotate-all)
            ROTATE_ON_START=true
            rotate_if_needed "$LOG_FILE"
            rotate_if_needed "$ERROR_LOG"
            rotate_if_needed "$AUDIT_LOG" ;;
        write)
            local level="${CMD_ARGS[0]:-INFO}"
            local module="${CMD_ARGS[1]:-core}"
            local msg="${CMD_ARGS[2]:-}"
            write_log "$level" "$module" "$msg" ;;
        set-level)
            set_log_level "${CMD_ARGS[0]:-INFO}" ;;
        get-level)
            get_log_level ;;
        prune)
            _prune_archives ;;
        status)
            print_status ;;
        help|--help|-h)
            usage ;;
        *)
            _mlog ERROR "Unknown command: $COMMAND"
            usage
            exit $EXIT_FAIL ;;
    esac
}

main "$@"
