# MedorCoin Blockchain — Protocol Specification

Version: 1.1.0
Network: Mainnet
Chain ID: 1337
Repository: MedorCoin-Blockchain

---

## Changelog

| Version | Date       | Summary                                              |
|---------|------------|------------------------------------------------------|
| 1.0.0   | 2025-01-01 | Initial specification                                |
| 1.1.0   | 2025-03-01 | Added block size limits, gas limits, fork/reorg rules, replay protection, serialization formats, security considerations |

---

## 1. Overview

MedorCoin is a decentralised blockchain combining Bitcoin-style UTXO tracking
with Ethereum-style account balances and EIP-1559 fee mechanics.

The protocol is designed for:
- Deterministic block production with Proof of Work
- Hybrid UTXO and account-balance state model
- EIP-1559 dynamic base fee adjustment
- Replay-protected transactions across all chain IDs
- Binary-serialized wire format for inter-node compatibility

---

## 2. Block Structure

| Field         | Type     | Size     | Description                          |
|---------------|----------|----------|--------------------------------------|
| previousHash  | bytes32  | 32 bytes | Keccak256 hash of previous block     |
| hash          | bytes32  | 32 bytes | Keccak256 hash of current block      |
| timestamp     | uint64   | 8 bytes  | Unix timestamp in seconds            |
| difficulty    | uint32   | 4 bytes  | PoW difficulty target                |
| nonce         | uint64   | 8 bytes  | PoW solution nonce                   |
| minerAddress  | bytes20  | 20 bytes | Address of block miner               |
| reward        | uint64   | 8 bytes  | Block reward in base units           |
| baseFee       | uint64   | 8 bytes  | EIP-1559 base fee per gas            |
| gasUsed       | uint64   | 8 bytes  | Total gas used in block              |
| gasLimit      | uint64   | 8 bytes  | Maximum gas allowed per block        |
| txCount       | uint32   | 4 bytes  | Number of transactions in block      |
| transactions  | array    | variable | List of serialized transactions      |

### 2.1 Block Size Limits

| Parameter              | Value        |
|------------------------|--------------|
| Maximum block size     | 2 MB         |
| Maximum gas per block  | 30,000,000   |
| Minimum gas per block  | 0            |
| Target gas per block   | 15,000,000   |
| Maximum transactions   | 5,000        |

A block exceeding 2 MB in serialized size or 30,000,000 gas MUST be rejected
by all nodes regardless of PoW validity. Miners MUST NOT produce blocks that
exceed these limits. The gas limit per block is a soft target adjusted by
miners within ±1/1024 per block, bounded by the hard maximum.

---

## 3. Transaction Structure

| Field                  | Type     | Size     | Description                    |
|------------------------|----------|----------|--------------------------------|
| txHash                 | bytes32  | 32 bytes | Keccak256 hash of transaction  |
| chainId                | uint64   | 8 bytes  | Chain ID (1337 for mainnet)    |
| nonce                  | uint64   | 8 bytes  | Sender nonce                   |
| toAddress              | bytes20  | 20 bytes | Recipient address              |
| value                  | uint64   | 8 bytes  | Amount in base units           |
| gasLimit               | uint64   | 8 bytes  | Maximum gas for transaction    |
| maxFeePerGas           | uint64   | 8 bytes  | EIP-1559 max fee per gas       |
| maxPriorityFeePerGas   | uint64   | 8 bytes  | EIP-1559 priority tip          |
| inputCount             | uint32   | 4 bytes  | Number of UTXO inputs          |
| inputs                 | array    | variable | UTXO inputs                    |
| outputCount            | uint32   | 4 bytes  | Number of UTXO outputs         |
| outputs                | array    | variable | UTXO outputs                   |
| v                      | uint8    | 1 byte   | Signature recovery id          |
| r                      | bytes32  | 32 bytes | Signature r component          |
| s                      | bytes32  | 32 bytes | Signature s component          |

### 3.1 Per-Transaction Gas Limits

| Parameter                   | Value      |
|-----------------------------|------------|
| Minimum gas per transaction | 21,000     |
| Maximum gas per transaction | 10,000,000 |
| Gas per UTXO input          | 68         |
| Gas per UTXO output         | 34         |
| Gas per byte of data        | 16         |

A transaction with gasLimit below 21,000 or above 10,000,000 MUST be
rejected at the mempool and consensus layer. Nodes MUST NOT relay
transactions that exceed per-transaction gas limits.

### 3.2 UTXO Input Structure

| Field       | Type    | Size     | Description                      |
|-------------|---------|----------|----------------------------------|
| txHash      | bytes32 | 32 bytes | Hash of the source transaction   |
| outputIndex | uint32  | 4 bytes  | Index of the output being spent  |

### 3.3 UTXO Output Structure

| Field   | Type    | Size     | Description                      |
|---------|---------|----------|----------------------------------|
| address | bytes20 | 20 bytes | Recipient address                |
| amount  | uint64  | 8 bytes  | Amount in base units             |

---

## 4. Consensus — Proof of Work

- Algorithm: Keccak256
- Block target time: 10 seconds
- Difficulty adjustment: every block, ±12.5% max change
- Minimum difficulty: 1
- Maximum difficulty: 64

### 4.1 Fork Rules

MedorCoin follows the **heaviest chain rule** (total accumulated difficulty):

1. The canonical chain is the chain with the greatest total accumulated
   difficulty, not simply the longest chain by block count.
2. On difficulty tie, the chain whose tip was received first is preferred.
3. Nodes MUST track and store the total accumulated difficulty of every
   chain tip they are aware of.

Protocol upgrades are activated by **block height** hard forks. Each upgrade
activation height is published as part of a network-wide coordinated release.
Nodes running software that does not recognise an upgrade height MUST be
considered non-canonical after that height.

### 4.2 Reorganisation (Reorg) Handling

A reorg occurs when a competing chain accumulates more total difficulty than
the current canonical chain.

**Reorg procedure:**

1. Walk back the canonical chain from its current tip to the common ancestor
   with the competing chain.
2. Roll back all state changes (UTXO set, account balances, spent outpoints,
   nonces) for each block being removed, in reverse order.
3. Apply all blocks from the common ancestor to the new chain tip in forward
   order, validating each block fully.
4. Return all transactions from removed blocks to the mempool, excluding
   any that conflict with the new canonical chain.

**Reorg depth limits:**

| Parameter             | Value      |
|-----------------------|------------|
| Maximum safe reorg    | 100 blocks |
| Deep reorg threshold  | 100 blocks |

Reorgs deeper than 100 blocks MUST be treated as a potential network attack
or severe consensus failure. Nodes SHOULD alert operators and MAY halt
automatic chain switching pending manual review when a reorg exceeds this
depth.

---

## 5. Block Reward Schedule

| Period                  | Reward per Block |
|-------------------------|------------------|
| First 60 days           | 55 MEDOR         |
| After 60 days           | 30 MEDOR         |

Reward split per block:
- Miner: 90%
- Owner: 10%

Maximum supply: 50,000,000 MEDOR

---

## 6. Network Fee Structure

All non-coinbase transactions are subject to a network fee:

| Transaction Value       | Fee                        |
|-------------------------|----------------------------|
| 0 to 1000 MEDOR         | Flat 0.009 MEDOR           |
| Above 1000 MEDOR        | 2% of transaction value    |

- Fee is deducted from recipient
- Fee is credited to treasury address
- Coinbase (mining reward) transactions are exempt

---

## 7. EIP-1559 Fee Mechanics

- Base fee adjusts per block based on gas usage
- If gasUsed > 50% of gasLimit: base fee increases by 12.5%
- If gasUsed < 50% of gasLimit: base fee decreases by 12.5%
- Minimum base fee: 1
- Effective fee = min(maxFeePerGas, baseFee + maxPriorityFeePerGas)

---

## 8. UTXO Model

MedorCoin uses a hybrid UTXO and account model:

- UTXOs track unspent outputs for each transaction
- Account balances track running totals per address
- Coinbase maturity: 100 blocks before coinbase UTXO can be spent
- Double spend prevention via spent outpoint tracking

---

## 9. Cryptography

- Key generation: secp256k1 via libsecp256k1
- Transaction hashing: Keccak256 (Ethereum style)
- Block hashing: Keccak256
- Address derivation: Keccak256(pubkey)[12:32] — last 20 bytes
- Signature: ECDSA recoverable signature (r, s, v)
- BIP39 mnemonic support for wallet derivation

---

## 10. Serialization Format

All data transmitted over the P2P wire protocol and written to disk uses a
deterministic binary serialization format. JSON is used only for the RPC API
layer (Section 11).

### 10.1 Encoding Rules

| Type    | Encoding                                           |
|---------|----------------------------------------------------|
| uint8   | 1 byte, big-endian                                 |
| uint32  | 4 bytes, big-endian                                |
| uint64  | 8 bytes, big-endian                                |
| bytes20 | 20 bytes, raw                                      |
| bytes32 | 32 bytes, raw                                      |
| array   | uint32 element count followed by concatenated elements |
| bool    | 1 byte: 0x00 = false, 0x01 = true                  |

All multi-byte integers are **big-endian**. Little-endian encodings are not
valid and MUST be rejected.

### 10.2 Block Serialization Order

Fields MUST be serialized in the exact order listed in Section 2, using the
encodings in Section 10.1. The `txCount` field MUST equal the number of
transaction entries that follow. The block hash is computed over the
serialized header only (all fields except `transactions`).

### 10.3 Transaction Serialization Order

Fields MUST be serialized in the exact order listed in Section 3, using the
encodings in Section 10.1. `inputCount` MUST precede the `inputs` array,
and `outputCount` MUST precede the `outputs` array. The transaction hash is
computed over all fields except `txHash` itself.

### 10.4 Signed Transaction Hashing

The transaction hash used for signing excludes `v`, `r`, and `s`. The
signing payload is:

