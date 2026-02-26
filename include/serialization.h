#pragma once

#include <nlohmann/json.hpp>
#include "block.h"
#include "transaction.h"

nlohmann::json serializeBlock(const Block &block);
Block deserializeBlock(const nlohmann::json &j);

nlohmann::json serializeTx(const Transaction &tx);
Transaction deserializeTx(const nlohmann::json &j);
