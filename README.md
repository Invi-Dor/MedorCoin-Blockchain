# MedorCoin Blockchain

MedorCoin is a secure, high‑performance, multifunctional blockchain with advanced token capabilities, liquidity controls, and enterprise‑grade release verification.

## 🔐 Verified Releases

[![Verified Release](https://img.shields.io/badge/release%20verified-GPG%20signed-brightgreen)](https://github.com/<your‑username>/<your‑repo>/releases)

All official MedorCoin releases are **cryptographically signed**. Follow the instructions below to verify authenticity.

## 🛠️ Quick Install (CLI)

You can import the official MedorCoin public key and verify releases quickly:

```sh
#!/bin/sh
# Import the MedorCoin public GPG key
curl -sSL https://raw.githubusercontent.com/<your‑username>/<your‑repo>/main/keys/medorcoin-public.asc \
  | gpg --import

echo "Public key imported. You can now verify MedorCoin release signatures."
