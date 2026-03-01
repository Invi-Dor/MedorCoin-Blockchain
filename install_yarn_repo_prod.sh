#!/usr/bin/env bash
set -euo pipefail

LOG="/var/log/install_yarn_repo.log"
KEYRING="/usr/share/keyrings/yarn-archive-keyring.gpg"
YARN_LIST="/etc/apt/sources.list.d/yarn.list"
PUBKEY_URL="https://dl.yarnpkg.com/debian/pubkey.gpg"

exec > >(tee -a "$LOG") 2>&1

if [ "$EUID" -ne 0 ]; then
  echo "This script must be run as root. Re-run with sudo." >&2
  exit 1
fi

mkdir -p /usr/share/keyrings
rm -f "$KEYRING" "$YARN_LIST"

echo "Fetching Yarn signing key (curl, fallback to wget)..."
if command -v curl >/dev/null 2>&1 && curl -fsSL "$PUBKEY_URL" | gpg --dearmor -o "$KEYRING"; then
  :
elif command -v wget >/dev/null 2>&1 && wget -qO- "$PUBKEY_URL" | gpg --dearmor -o "$KEYRING"; then
  :
else
  echo "ERROR: Failed to fetch and dearmor Yarn key." >&2
  exit 1
fi

DEB_LINE="deb [signed-by=$KEYRING] https://dl.yarnpkg.com/debian/ stable main"
echo "$DEB_LINE" > "$YARN_LIST"

echo "Updating apt package lists..."
apt-get update -y

echo "Validation: key present and signed-by reference set."
if [ -f "$KEYRING" ] && grep -q "signed-by=$KEYRING" "$YARN_LIST"; then
  echo "Validation passed."
else
  echo "WARNING: Validation may be incomplete. See log at $LOG" >&2
fi

echo "Done. You can install Yarn with: apt-get install -y yarn"
