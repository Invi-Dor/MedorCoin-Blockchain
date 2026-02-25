#pragma once

#include <string>
#include "block.h"

/**
 * Sign a block for PoA consensus.
 *
 * @param block The block to sign
 * @param validatorAddr Hex address of the validator (40 chars)
 * @return hex‑encoded signature string (r + s + v)
 */
std::string signBlockPoA(const Block &block, const std::string &validatorAddr);

/**
 * Verify PoA block signature.
 *
 * @param block The block with signature field set
 * @return true if the signature is valid and from an authorized validator
 */
bool verifyBlockPoA(const Block &block);
