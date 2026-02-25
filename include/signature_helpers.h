#pragma once
#include <cstdint>

/**
 * Compute the EIP‑155 `v` value for an EVM transaction.
 *
 * According to Ethereum Improvement Proposal 155,
 * v MUST be: recid + 35 + 2 * chainId
 *
 * Where:
 *   - recid is the ECDSA recovery id (0 or 1)
 *   - chainId is the unique identifier for your blockchain
 *
 * This prevents replay attacks across chains.  [oai_citation:0‡Ethereum Improvement Proposals](https://eips.ethereum.org/EIPS/eip-155?utm_source=chatgpt.com)
 */
uint8_t computeEip155V(uint8_t recid, uint64_t chainId);
