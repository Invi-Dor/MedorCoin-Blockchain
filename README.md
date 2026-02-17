MedorCoin Blockchain

MedorCoin is a secure, high‑performance, multifunctional blockchain focused on strong cryptography, advanced token features, liquidity controls, and enterprise‑grade release verification. Security is the project’s top priority.

Features

- Advanced token mechanics and liquidity controls
- Secure networking and crypto primitives (OpenSSL, secp256k1)
- Durable storage via RocksDB
- GPG‑signed releases with reproducible verification steps
- CMake‑based build for Linux/macOS/Windows

Verified Releases

![Verified Release](https://github.com/<your-username>/<your-repo>/releases)

All official MedorCoin releases are cryptographically signed. Always verify signatures before installing or running binaries.

Quick Verify (CLI)

Replace <your-username>/<your-repo> with your repository path.

#!/usr/bin/env sh
set -e

# 1) Import the MedorCoin public GPG key
curl -sSL https://raw.githubusercontent.com/<your-username>/<your-repo>/main/keys/medorcoin-public.asc \
  | gpg --import

# 2) Download assets (example)
VERSION="v0.1.0"
curl -sSLO https://github.com/<your-username>/<your-repo>/releases/download/${VERSION}/medorcoin-${VERSION}-linux-x86_64.tar.gz
curl -sSLO https://github.com/<your-username>/<your-repo>/releases/download/${VERSION}/medorcoin-${VERSION}-linux-x86_64.tar.gz.sig

# 3) Verify signature
gpg --verify medorcoin-${VERSION}-linux-x86_64.tar.gz.sig medorcoin-${VERSION}-linux-x86_64.tar.gz

echo "Signature OK. You can safely extract and run this release."

Build from Source

Prerequisites:
- CMake ≥ 3.16
- A C++20 compiler (GCC 10+/Clang 12+/MSVC 2019+)
- OpenSSL (libssl, libcrypto)
- RocksDB
- libsecp256k1 (pkg-config: libsecp256k1)
- Threads/pthreads
- Optional: zlib, bzip2, lz4, zstd, snappy

Linux/macOS:

# Install deps (examples; adjust for your distro)
# Ubuntu/Debian:
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  libssl-dev librocksdb-dev libsecp256k1-dev \
  zlib1g-dev libbz2-dev liblz4-dev libzstd-dev libsnappy-dev

# Clone and build
git clone https://github.com/<your-username>/<your-repo>.git
cd <your-repo>
cmake -S . -B build -DENABLE_WARNINGS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Binary:
./build/bin/medorcoin

Homebrew (macOS):

brew install cmake openssl@3 rocksdb libsecp256k1 lz4 zstd snappy bzip2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build -j

Windows (MSVC + vcpkg example):

# Powershell
git clone https://github.com/<your-username>/<your-repo>.git
cd <your-repo>

# Install deps via vcpkg (example triplet x64-windows)
vcpkg install openssl rocksdb secp256k1 zlib bzip2 lz4 zstd snappy --triplet x64-windows

# Configure CMake with vcpkg toolchain
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Binary:
.\build\bin\medorcoin.exe

Usage

- Run the node:
  - Linux/macOS: ./build/bin/medorcoin
  - Windows: .\build\bin\medorcoin.exe
- See --help for supported flags once implemented.

Security Notes

- Verify GPG signatures for every release.
- Prefer building from source in high‑security environments.
- Keep keys and wallets offline whenever possible, and verify addresses before sending.

Support & Sponsorship

If you appreciate the work on MedorCoin‑Blockchain and want to support ongoing development:

Platforms you can send to:
1. Ethereum: 0x85708d61FEfcbb6eb13C72b0D42bCeB246F06dd0
2. Bitcoin: bc1qlw9h0nnde9yewacdkfetcym96l24ucu5ld9atv
3. Solana: Hy2sTujvdnSE6r1K1tigfjVLbf666RkHQQMRcLc24HBE
4. BNB Chain: 0x85708d61FEfcbb6eb13C72b0D42bCeB246F06dd0

Your sponsorship helps cover development time, infrastructure, security audits, and better tooling/documentation.

Why Sponsor?

- Improve performance and security
- Add consensus and networking features
- Enhance privacy and robustness
- Better docs, tooling, and community support

Thank You

Every contribution—big or small—is appreciated. Thanks for supporting open‑source innovation and the vision behind MedorCoin.

Disclaimer

MedorCoin‑Blockchain is provided as‑is under an open‑source license. Sponsorship is not an investment and offers no guarantees of financial return, ownership, or rights. Always do your own research and use blockchain software responsibly and securely. Verify all releases and addresses before transacting.
