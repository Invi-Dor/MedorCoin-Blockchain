#pragma once

#include "block.h"
#include <vector>

// =============================================================================
// BLOCKCHAIN FORK RESOLUTION
//
// Implements the longest-chain rule for MedorCoin fork resolution.
//
// resolveLongestChain() is called by Blockchain::resolveFork() under an
// exclusive write lock. It replaces the current chain with the candidate
// chain if and only if the candidate is strictly longer and every block
// in the candidate passes structural validity checks.
//
// Thread safety:
//   Blockchain::resolveFork() holds a unique_lock on rwMutex_ before
//   calling this function. This function does not lock internally to
//   avoid double-locking the same mutex.
//
// Memory:
//   Replacement vector is built fully via clone() before currentChain
//   is touched so currentChain is never left in a partial state on error.
//   For mainnet scale pair with BlockDB pruning to keep the in-memory
//   vector bounded.
//
// Efficiency:
//   Validation is O(n) over the candidate with early exit on first failure.
//   For very large chains consider checkpoint validation or a hash index
//   to reduce the validation window.
//
// Returns true  -- currentChain was replaced with candidateChain.
// Returns false -- currentChain was not modified.
// Never throws.
// =============================================================================

bool resolveLongestChain(const std::vector<Block> &candidateChain,
                          std::vector<Block>        &currentChain) noexcept;
