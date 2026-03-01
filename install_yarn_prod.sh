#!/usr/bin/env bash
set -euo pipefail

LOG="/var/log/install_yarn_prod.log"
KEYRING="/usr/share/keyrings/yarn-archive-keyring.gpg"
YARN_LIST="/etc/apt/sources.list.d/yarn.list"
PUBKEY_URL="https://dl.yarnpkg.com/debian/pubkey.gpg"

exec > >(tee -a "$LOG") 2>&1

if [ "$EUID" -ne 0 ]; then
  echo "This script must be run as root. Re-run with sudo." >&2
  exit 1
fi

# --- Setup Yarn repository ---
mkdir -p /usr/share/keyrings
rm -f "$KEYRING" "$YARN_LIST"

echo "Fetching Yarn GPG key..."
if command -v curl >/dev/null 2>&1 && curl -fsSL "$PUBKEY_URL" | gpg --dearmor -o "$KEYRING"; then
  :
elif command -v wget >/dev/null 2>&1 && wget -qO- "$PUBKEY_URL" | gpg --dearmor -o "$KEYRING"; then
  :
else
  echo "ERROR: Failed to fetch Yarn key." >&2
  exit 1
fi

echo "Adding Yarn repository..."
echo "deb [signed-by=$KEYRING] https://dl.yarnpkg.com/debian/ stable main" > "$YARN_LIST"

echo "Updating apt package lists..."
apt-get update -y

# --- Install Yarn ---
echo "Installing latest stable Yarn..."
apt-get install -y --no-install-recommends yarn

# --- Validation ---
if command -v yarn >/dev/null 2>&1; then
  echo "Yarn installed successfully: $(yarn --version)"
else
  echo "ERROR: Yarn installation failed." >&2
  exit 1
fi

echo "Done. Yarn is ready to use."
