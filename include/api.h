#ifndef MEDOR_API_H
#define MEDOR_API_H

void startAPIServer();

#endif

// API.h
#pragma once
#include <string>
#include <vector>
#include "utxo.h"

std::vector<UTXO> getUTXOs(const std::string& address);
