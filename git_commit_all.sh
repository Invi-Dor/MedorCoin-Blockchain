#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════════════
# git_commit_all.sh
#
# Stages every new, modified, and deleted file not covered by .gitignore,
# then commits with a timestamped message. Run from the repository root.
#
# Usage:
#   chmod +x git_commit_all.sh
#   ./git_commit_all.sh
#   ./git_commit_all.sh --message "your custom commit message"
#   ./git_commit_all.sh --dry-run
#   ./git_commit_all.sh --push
# ══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ── Colour output ─────────────────────────────────────────────────────────────
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

# ── Argument parsing ──────────────────────────────────────────────────────────
DRY_RUN=false
AUTO_PUSH=false
COMMIT_MSG=""

for ARG in "$@"; do
    case "${ARG}" in
        --dry-run)       DRY_RUN=true ;;
        --push)          AUTO_PUSH=true ;;
        --message=*)     COMMIT_MSG="${ARG#--message=}" ;;
        --help)
            echo "Usage: $0 [--dry-run] [--push] [--message=\"your message\"]"
            exit 0
            ;;
        *)
            fatal "Unknown argument: ${ARG}. Run with --help for usage."
            ;;
    esac
done

# ── Verify git is available and we are inside a repository ───────────────────
command -v git &>/dev/null || fatal "git is not installed or not in PATH."

git rev-parse --is-inside-work-tree &>/dev/null || \
    fatal "Not inside a git repository. Run this script from the repository root."

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"
info "Repository root: ${REPO_ROOT}"

# ── Verify .gitignore exists and warn if .env would be committed ──────────────
if [[ ! -f ".gitignore" ]]; then
    warn ".gitignore not found. All files including secrets may be staged."
    warn "Create .gitignore before continuing."
    read -r -p "Continue anyway? Type YES to proceed: " CONFIRM_NO_IGNORE
    [[ "${CONFIRM_NO_IGNORE}" == "YES" ]] || \
        { info "Aborted. No changes were made."; exit 0; }
fi

# Hard guard — refuse to proceed if .env would be staged
if git check-ignore -q ".env" 2>/dev/null; then
    : # .env is correctly ignored
else
    if [[ -f ".env" ]]; then
        fatal ".env exists and is NOT covered by .gitignore.\n" \
              "       Add '.env' to .gitignore before running this script.\n" \
              "       echo '.env' >> .gitignore"
    fi
fi

# ── Show current repository status ───────────────────────────────────────────
echo ""
info "Current repository status:"
echo ""
git status --short
echo ""

# ── Count changes ─────────────────────────────────────────────────────────────
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
    success "Nothing to commit. The working tree is clean and up to date."
    exit 0
fi

# ── Dry-run mode ──────────────────────────────────────────────────────────────
if [[ "${DRY_RUN}" == "true" ]]; then
    echo ""
    warn "DRY-RUN MODE — the following files would be staged and committed:"
    echo ""
    git ls-files --others --exclude-standard
    git ls-files --modified
    git ls-files --deleted
    echo ""
    warn "Remove --dry-run to apply these changes."
    exit 0
fi

# ── Confirm before staging ────────────────────────────────────────────────────
read -r -p "Stage and commit all ${TOTAL_CHANGES} change(s)? Type YES to continue: " CONFIRM
[[ "${CONFIRM}" == "YES" ]] || { info "Aborted. No changes were made."; exit 0; }

# ── Stage all changes ─────────────────────────────────────────────────────────
echo ""
info "Staging all changes..."
git add --all

# Show exactly what has been staged
echo ""
info "Files staged for commit:"
echo ""
git diff --cached --name-status
echo ""

# ── Build the commit message ──────────────────────────────────────────────────
if [[ -z "${COMMIT_MSG}" ]]; then
    TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
    BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
    COMMIT_MSG="chore: update ${TOTAL_CHANGES} file(s) on ${BRANCH} at ${TIMESTAMP}"
fi

# ── Commit ────────────────────────────────────────────────────────────────────
info "Committing with message: \"${COMMIT_MSG}\""
echo ""

git commit -m "${COMMIT_MSG}"

echo ""
success "Commit created successfully."

# ── Show the commit that was just created ─────────────────────────────────────
echo ""
info "Commit details:"
git show --stat HEAD
echo ""

# ── Optional push ─────────────────────────────────────────────────────────────
REMOTE="$(git remote 2>/dev/null | head -n1 || true)"
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'main')"

if [[ "${AUTO_PUSH}" == "true" ]]; then
    if [[ -z "${REMOTE}" ]]; then
        warn "No remote configured. Skipping push."
    else
        info "Pushing to ${REMOTE}/${BRANCH}..."
        git push "${REMOTE}" "${BRANCH}"
        success "Push complete."
    fi
else
    if [[ -n "${REMOTE}" ]]; then
        echo ""
        read -r -p "Push to ${REMOTE}/${BRANCH} now? Type YES to push: " PUSH_CONFIRM
        if [[ "${PUSH_CONFIRM}" == "YES" ]]; then
            git push "${REMOTE}" "${BRANCH}"
            success "Push complete."
        else
            info "Push skipped. Push manually when ready:"
            info "  git push ${REMOTE} ${BRANCH}"
        fi
    else
        warn "No remote repository configured."
        warn "Add one with: git remote add origin <repository-url>"
    fi
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════════"
success "ALL DONE"
echo ""
echo "  Files committed : ${TOTAL_CHANGES}"
echo "  Branch          : ${BRANCH}"
echo "  Commit message  : ${COMMIT_MSG}"
echo ""
echo "  To review the full commit history:"
echo "    git log --oneline -20"
echo "══════════════════════════════════════════════════════════════════"
echo ""
