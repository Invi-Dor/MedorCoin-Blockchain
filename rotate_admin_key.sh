#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
# rotate_admin_key.sh
#
# Generates a new MEDOR_ADMIN_KEY, replaces it in every file that references
# the old key across the repository, verifies removal, and optionally restarts
# the MedorCoin API service.
#
# Usage:
#   chmod +x rotate_admin_key.sh
#   ./rotate_admin_key.sh                          # standard interactive mode
#   ./rotate_admin_key.sh --dry-run                # preview changes only
#   ./rotate_admin_key.sh --auto-restart           # restart service when done
#   ./rotate_admin_key.sh --env-file /path/to/.env # specify env file location
#
# Platform notes:
#   Linux  : fully supported including systemctl service restart.
#   macOS  : fully supported; service restart step is skipped (no systemd).
#   Windows: run inside WSL2 or Git Bash; service restart is skipped.
#
# Requirements: openssl, grep, sed, mktemp (all standard on Linux and macOS).
# ══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Colour codes — suppressed automatically when output is not a terminal
# ─────────────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    RED="\033[0;31m"; YELLOW="\033[0;33m"
    GREEN="\033[0;32m"; CYAN="\033[0;36m"; RESET="\033[0m"
else
    RED=""; YELLOW=""; GREEN=""; CYAN=""; RESET=""
fi

info()    { echo -e "${CYAN}[INFO]${RESET}    $*"; }
success() { echo -e "${GREEN}[SUCCESS]${RESET} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}    $*"; }
error()   { echo -e "${RED}[ERROR]${RESET}   $*" >&2; }
fatal()   { error "$*"; exit 1; }

# ─────────────────────────────────────────────────────────────────────────────
# Argument parsing
# ─────────────────────────────────────────────────────────────────────────────
DRY_RUN=false
AUTO_RESTART=false
CUSTOM_ENV_FILE=""
SYSTEMD_SERVICE="medorcoin-blockchain"

for ARG in "$@"; do
    case "${ARG}" in
        --dry-run)        DRY_RUN=true ;;
        --auto-restart)   AUTO_RESTART=true ;;
        --env-file=*)     CUSTOM_ENV_FILE="${ARG#--env-file=}" ;;
        --service=*)      SYSTEMD_SERVICE="${ARG#--service=}" ;;
        --help)
            sed -n '2,/^# Requirements/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            fatal "Unknown argument: ${ARG}\n       Run with --help for usage."
            ;;
    esac
done

# ─────────────────────────────────────────────────────────────────────────────
# Verify the script is running from the repository root
# ─────────────────────────────────────────────────────────────────────────────
if [[ ! -f "CMakeLists.txt" ]] && [[ ! -f ".gitignore" ]] && [[ ! -d ".git" ]]; then
    fatal "This script must be run from the MedorCoin repository root.\n" \
          "       Change directory to the repo root and try again:\n" \
          "       cd /path/to/MedorCoin-Blockchain && ./rotate_admin_key.sh"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Dry-run banner
# ─────────────────────────────────────────────────────────────────────────────
if [[ "${DRY_RUN}" == "true" ]]; then
    echo ""
    echo "══════════════════════════════════════════════════════════════════"
    warn "DRY-RUN MODE — no files will be created, modified, or deleted."
    warn "Remove --dry-run from the command to apply changes for real."
    echo "══════════════════════════════════════════════════════════════════"
    echo ""
fi

# ─────────────────────────────────────────────────────────────────────────────
# Locate and validate the environment file
#
# Priority: --env-file flag → .env → .env.example
# ─────────────────────────────────────────────────────────────────────────────
if [[ -n "${CUSTOM_ENV_FILE}" ]]; then
    [[ -f "${CUSTOM_ENV_FILE}" ]] || \
        fatal "Specified env file not found: ${CUSTOM_ENV_FILE}"
    ENV_FILE="${CUSTOM_ENV_FILE}"
elif [[ -f ".env" ]]; then
    ENV_FILE=".env"
elif [[ -f ".env.example" ]]; then
    ENV_FILE=".env.example"
    warn "No .env file found; reading from .env.example."
    warn "Create a .env file from .env.example before deploying:"
    warn "  cp .env.example .env"
else
    fatal "No environment file found. Create .env from the template:\n" \
          "       cp .env.example .env\n" \
          "       Then set MEDOR_ADMIN_KEY to a 64-character hex value:\n" \
          "       MEDOR_ADMIN_KEY=\$(openssl rand -hex 32)"
fi

info "Reading current admin key from: ${ENV_FILE}"

# ─────────────────────────────────────────────────────────────────────────────
# Extract and validate the current admin key
# ─────────────────────────────────────────────────────────────────────────────
OLD_KEY="$(grep -E '^MEDOR_ADMIN_KEY=' "${ENV_FILE}" \
    | head -n1 \
    | sed 's/^MEDOR_ADMIN_KEY=//' \
    | tr -d '[:space:]')"

if [[ -z "${OLD_KEY}" ]]; then
    fatal "MEDOR_ADMIN_KEY not found in ${ENV_FILE}.\n" \
          "       Add a line in the form:\n" \
          "       MEDOR_ADMIN_KEY=\$(openssl rand -hex 32)"
fi

# Reject placeholder values that were never replaced
if [[ "${OLD_KEY}" == *"REPLACE"* ]] || [[ "${OLD_KEY}" == *"<"* ]]; then
    fatal "MEDOR_ADMIN_KEY in ${ENV_FILE} still contains a placeholder value.\n" \
          "       Replace it with a real 64-character hex key:\n" \
          "       MEDOR_ADMIN_KEY=\$(openssl rand -hex 32)"
fi

if [[ ${#OLD_KEY} -ne 64 ]] || ! [[ "${OLD_KEY}" =~ ^[0-9a-fA-F]+$ ]]; then
    fatal "MEDOR_ADMIN_KEY in ${ENV_FILE} is invalid.\n" \
          "       Expected exactly 64 hexadecimal characters.\n" \
          "       Got ${#OLD_KEY} characters: ${OLD_KEY:0:8}..."
fi

info "Current key validated (first 8 chars): ${OLD_KEY:0:8}..."

# ─────────────────────────────────────────────────────────────────────────────
# Generate the new key
# ─────────────────────────────────────────────────────────────────────────────
NEW_KEY="$(openssl rand -hex 32)"
[[ ${#NEW_KEY} -eq 64 ]] || fatal "openssl rand failed to produce a 64-character key."

echo ""
echo "══════════════════════════════════════════════════════════════════"
echo "  NEW ADMIN KEY — record this in your secrets manager before"
echo "  continuing. It will not be displayed again."
echo ""
echo "  ${NEW_KEY}"
echo ""
echo "══════════════════════════════════════════════════════════════════"
echo ""

if [[ "${DRY_RUN}" == "false" ]]; then
    read -r -p "Have you recorded the new key securely? Type YES to continue: " CONFIRM_KEY
    [[ "${CONFIRM_KEY}" == "YES" ]] || \
        { info "Aborted by user. No files were modified."; exit 0; }
fi

# ─────────────────────────────────────────────────────────────────────────────
# Scan for affected files
# ─────────────────────────────────────────────────────────────────────────────
echo ""
info "Scanning repository for files containing the old key..."

AFFECTED_FILES="$(grep -rl "${OLD_KEY}" . \
    --exclude-dir=.git \
    --exclude-dir=node_modules \
    --exclude-dir=build \
    --exclude-dir=rocksdb_build \
    2>/dev/null || true)"

if [[ -z "${AFFECTED_FILES}" ]]; then
    info "No files found containing the old key. Nothing to do."
    exit 0
fi

echo ""
info "Files that contain the old key:"
while IFS= read -r FILE; do
    echo "    ${FILE}"
done <<< "${AFFECTED_FILES}"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Dry-run exit — show what would happen and stop
# ─────────────────────────────────────────────────────────────────────────────
if [[ "${DRY_RUN}" == "true" ]]; then
    warn "DRY-RUN: the $(wc -l <<< "${AFFECTED_FILES}" | tr -d ' ') file(s)" \
         "listed above would be updated."
    warn "DRY-RUN: no changes have been applied."
    warn "Remove --dry-run to execute the rotation for real."
    echo ""
    exit 0
fi

read -r -p "Replace the old key in all listed files? Type YES to continue: " CONFIRM_REPLACE
[[ "${CONFIRM_REPLACE}" == "YES" ]] || \
    { info "Aborted by user. No files were modified."; exit 0; }

# ─────────────────────────────────────────────────────────────────────────────
# Back up every affected file
# ─────────────────────────────────────────────────────────────────────────────
BACKUP_DIR=".key_rotation_backup_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${BACKUP_DIR}"
echo ""
info "Backing up affected files to ${BACKUP_DIR}/ ..."

while IFS= read -r FILE; do
    cp --parents "${FILE}" "${BACKUP_DIR}/"
    info "  backed up: ${FILE}"
done <<< "${AFFECTED_FILES}"

# ─────────────────────────────────────────────────────────────────────────────
# Replace the old key with the new key
# ─────────────────────────────────────────────────────────────────────────────
echo ""
info "Replacing old key with new key..."

while IFS= read -r FILE; do
    TMPFILE="$(mktemp)"
    # Uses a temp-file pattern rather than sed -i to remain fully compatible
    # with both GNU sed (Linux) and BSD sed (macOS) without flag differences.
    sed "s/${OLD_KEY}/${NEW_KEY}/g" "${FILE}" > "${TMPFILE}"
    mv "${TMPFILE}" "${FILE}"
    success "  updated: ${FILE}"
done <<< "${AFFECTED_FILES}"

# ─────────────────────────────────────────────────────────────────────────────
# Verify the old key no longer appears anywhere outside the backup directory
# ─────────────────────────────────────────────────────────────────────────────
echo ""
info "Verifying old key has been fully removed..."

REMAINING="$(grep -rl "${OLD_KEY}" . \
    --exclude-dir=.git \
    --exclude-dir=node_modules \
    --exclude-dir=build \
    --exclude-dir=rocksdb_build \
    --exclude-dir="${BACKUP_DIR}" \
    2>/dev/null || true)"

if [[ -n "${REMAINING}" ]]; then
    echo ""
    error "The old key still appears in the following files:"
    while IFS= read -r FILE; do
        error "  ${FILE}"
    done <<< "${REMAINING}"
    echo ""
    fatal "Rotation incomplete. Review the files above manually.\n" \
          "       Backups are preserved in ${BACKUP_DIR}/"
fi

success "Old key has been fully removed from all tracked files."

# ─────────────────────────────────────────────────────────────────────────────
# Protect .env from version control
# ─────────────────────────────────────────────────────────────────────────────
if [[ -f ".gitignore" ]]; then
    if ! grep -qE '^\.env$' ".gitignore"; then
        echo ".env" >> ".gitignore"
        info "Added .env to .gitignore."
    fi
else
    echo ".env" > ".gitignore"
    info "Created .gitignore and added .env."
fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage changed tracked files in git (optional)
# ─────────────────────────────────────────────────────────────────────────────
if command -v git &>/dev/null && git rev-parse --is-inside-work-tree &>/dev/null; then
    echo ""
    read -r -p "Stage modified tracked files in git now? Type YES to stage: " GIT_STAGE
    if [[ "${GIT_STAGE}" == "YES" ]]; then
        while IFS= read -r FILE; do
            # Only stage files git already tracks; .env must never be staged.
            if [[ "${FILE}" == "./.env" ]] || [[ "${FILE}" == ".env" ]]; then
                warn "  skipped (must never be committed): ${FILE}"
            elif git ls-files --error-unmatch "${FILE}" &>/dev/null 2>&1; then
                git add "${FILE}"
                info "  staged: ${FILE}"
            else
                warn "  skipped (untracked): ${FILE}"
            fi
        done <<< "${AFFECTED_FILES}"
        git add ".gitignore" 2>/dev/null || true
        echo ""
        info "Review staged changes:  git diff --cached"
        info "Commit when satisfied:  git commit -m 'chore: rotate admin key'"
        warn "Confirm .env is NOT staged before committing: git status"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Service restart
#
# Automated restart is available on Linux with systemd only. On macOS and
# Windows, a manual restart instruction is printed instead so the operator
# can use whichever process manager is in use on that platform.
# ─────────────────────────────────────────────────────────────────────────────
echo ""

PLATFORM="$(uname -s)"

if [[ "${AUTO_RESTART}" == "true" ]]; then
    if [[ "${PLATFORM}" == "Linux" ]] && command -v systemctl &>/dev/null; then
        info "Restarting ${SYSTEMD_SERVICE} via systemctl..."
        sudo systemctl restart "${SYSTEMD_SERVICE}"
        sleep 2
        STATUS="$(systemctl is-active "${SYSTEMD_SERVICE}" 2>/dev/null || echo 'unknown')"
        if [[ "${STATUS}" == "active" ]]; then
            success "${SYSTEMD_SERVICE} is running with the new key."
        else
            warn "${SYSTEMD_SERVICE} status is '${STATUS}' after restart."
            warn "Check the service logs:"
            warn "  sudo journalctl -u ${SYSTEMD_SERVICE} -n 50 --no-pager"
        fi
    elif [[ "${PLATFORM}" == "Darwin" ]]; then
        warn "--auto-restart is not supported on macOS (no systemd)."
        warn "Restart the service manually using your process manager, for example:"
        warn "  brew services restart medorcoin"
        warn "  launchctl kickstart -k gui/\$(id -u)/com.medorcoin.api"
    else
        warn "--auto-restart is not supported on this platform (${PLATFORM})."
        warn "Restart the MedorCoin API service manually."
    fi
else
    info "Service restart was not requested."
    if [[ "${PLATFORM}" == "Linux" ]] && command -v systemctl &>/dev/null; then
        info "Restart when ready:"
        info "  sudo systemctl restart ${SYSTEMD_SERVICE}"
    elif [[ "${PLATFORM}" == "Darwin" ]]; then
        info "Restart when ready (example for Homebrew-managed service):"
        info "  brew services restart medorcoin"
    else
        info "Restart the MedorCoin API service using your platform's"
        info "process manager before the new key takes effect."
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════════"
success "KEY ROTATION COMPLETE"
echo ""
echo "  New key : ${NEW_KEY}"
echo "  Backups : ${BACKUP_DIR}/"
echo ""
echo "  Remaining steps:"
echo "  1. Distribute the new key to all authorised admin clients."
echo "  2. Verify the service is healthy:"
echo "       curl -s https://api.medorcoin.io/health | python3 -m json.tool"
echo "  3. Once the service is confirmed healthy, delete the backup:"
echo "       rm -rf ${BACKUP_DIR}"
echo "══════════════════════════════════════════════════════════════════"
echo ""
