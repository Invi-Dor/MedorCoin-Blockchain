#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
# git_commit_all.sh
# ══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

if [[ -t 1 ]]; then
    RED="\033[0;31m"; YELLOW="\033[0;33m"
    GREEN="\033[0;32m"; CYAN="\033[0;36m"; RESET="\033[0m"
else
    RED=""; YELLOW=""; GREEN=""; CYAN=""; RESET=""
fi

info()    { echo -e "${CYAN}[INFO]${RESET}    $*"; }
success() { echo -e "${GREEN}[SUCCESS]${RESET} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}    $*"; }
fatal()   { echo -e "${RED}[ERROR]${RESET}   $*" >&2; exit 1; }

DRY_RUN=false
AUTO_PUSH=false
COMMIT_MSG=""

for ARG in "$@"; do
    case "${ARG}" in
        --dry-run)    DRY_RUN=true ;;
        --push)       AUTO_PUSH=true ;;
        --message=*)  COMMIT_MSG="${ARG#--message=}" ;;
        --help)
            echo "Usage: $0 [--dry-run] [--push] [--message=\"your message\"]"
            exit 0
            ;;
        *)
            fatal "Unknown argument: ${ARG}. Run with --help for usage."
            ;;
    esac
done

command -v git &>/dev/null || fatal "git is not installed or not in PATH."

git rev-parse --is-inside-work-tree &>/dev/null || \
    fatal "Not inside a git repository."

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"
info "Repository root: ${REPO_ROOT}"

if [[ ! -f ".gitignore" ]]; then
    warn ".gitignore not found. All files may be staged."
    read -r -p "Continue anyway? Type YES: " CONFIRM_NO_IGNORE
    [[ "${CONFIRM_NO_IGNORE}" == "YES" ]] || \
        { info "Aborted."; exit 0; }
fi

# ── .env warning only — not a hard block ─────────────────────────────────────
# This repository uses a two-file .env setup. The partial .env here links
# to a complete .env in a separate private repository. The check below
# warns if .env is not ignored but does not stop the script.
if [[ -f ".env" ]] && ! git check-ignore -q ".env" 2>/dev/null; then
    warn ".env exists and is not covered by .gitignore."
    warn "Continuing because this repository uses a linked .env setup."
fi

echo ""
info "Current repository status:"
echo ""
git status --short
echo ""

UNTRACKED="$(git ls-files --others --exclude-standard | wc -l | tr -d ' ')"
MODIFIED="$(git ls-files --modified | wc -l | tr -d ' ')"
DELETED="$(git ls-files --deleted | wc -l | tr -d ' ')"
STAGED="$(git diff --cached --name-only | wc -l | tr -d ' ')"

info "Untracked files : ${UNTRACKED}"
info "Modified files  : ${MODIFIED}"
info "Deleted files   : ${DELETED}"
info "Already staged  : ${STAGED}"
echo ""

TOTAL_CHANGES=$(( UNTRACKED + MODIFIED + DELETED + STAGED ))

if [[ "${TOTAL_CHANGES}" -eq 0 ]]; then
    success "Nothing to commit. The working tree is clean."
    exit 0
fi

if [[ "${DRY_RUN}" == "true" ]]; then
    warn "DRY-RUN MODE — files that would be committed:"
    git ls-files --others --exclude-standard
    git ls-files --modified
    git ls-files --deleted
    warn "Remove --dry-run to apply changes."
    exit 0
fi

read -r -p "Stage and commit all ${TOTAL_CHANGES} change(s)? Type YES: " CONFIRM
[[ "${CONFIRM}" == "YES" ]] || { info "Aborted."; exit 0; }

info "Staging all changes..."
git add --all

info "Files staged for commit:"
git diff --cached --name-status
echo ""

if [[ -z "${COMMIT_MSG}" ]]; then
    TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
    BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
    COMMIT_MSG="chore: update ${TOTAL_CHANGES} file(s) on ${BRANCH} at ${TIMESTAMP}"
fi

info "Committing: \"${COMMIT_MSG}\""
git commit -m "${COMMIT_MSG}"
success "Commit created successfully."
git show --stat HEAD

REMOTE="$(git remote 2>/dev/null | head -n1 || true)"
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'main')"

if [[ "${AUTO_PUSH}" == "true" ]]; then
    [[ -z "${REMOTE}" ]] && warn "No remote configured." || \
        { git push "${REMOTE}" "${BRANCH}" && success "Push complete."; }
else
    if [[ -n "${REMOTE}" ]]; then
        read -r -p "Push to ${REMOTE}/${BRANCH}? Type YES: " PUSH_CONFIRM
        [​​​​​​​​​​​​​​​​
