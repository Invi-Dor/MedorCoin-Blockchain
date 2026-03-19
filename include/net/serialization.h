#pragma once

#include "transaction.h"
#include "block.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

json        serializeTx(const Transaction& tx);
Transaction deserializeTx(const json& j);

json  serializeBlock(const Block& block);
Block deserializeBlock(const json& j);
