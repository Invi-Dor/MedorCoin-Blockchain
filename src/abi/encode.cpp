include "abi/encode.h"
include <sstream>
include <iomanip>
include <vector>

namespace crypto {

// Helper: convert hex string (e.g., "0xabcdef...") to raw bytes.
// This function is tolerant to or without "0x" prefix.
static std::vector<uint8_t> hexToBytes(const std::string& hex) {
  std::string s = hex;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
  }
  std::vector<uint8_t> bytes;
  if (s.empty()) return bytes;
  if (s.size() % 2 != 0) {
    // pad with leading zero if odd length
    std::string pref = "0";
    bytes.push_back((uint8_t)std::stoul(pref + s.substr(0,1), nullptr, 16));
    // rest
    for (size_t i = 1; i < s.size(); i += 2) {
      bytes.push_back((uint8_t)std::stoul(s.substr(i,2), nullptr, 16));
    }
  } else {
    for (size_t i = 0; i < s.size(); i += 2) {
      bytes.push_back((uint8_t)std::stoul(s.substr(i,2), nullptr, 16));
    }
  }
  return bytes;
}

std::vector<uint8_t> ABIEncoder::encodeAddress(const std::string& addr) {
  // Expect hex address (20 bytes). Pad to 32 bytes as per ABI encoding
  std::vector<uint8_t> raw = hexToBytes(addr);
  // Take last 20 bytes for address
  if (raw.size() > 20) raw = std::vector<uint8_t>(raw.end() - 20, raw.end());
  // Pad to 32 bytes
  std::vector<uint8_t> out(32, 0);
  if (raw.size() <= 32) {
    std::copy(raw.begin(), raw.end(), out.begin() + (32 - raw.size()));
  } else {
    // should not happen for a single address
    std::copy(raw.end() - 32, raw.end(), out.begin());
  }
  return out;
}

std::vector<uint8_t> ABIEncoder::encodeUint256(const std::string& value) {
  // Very simple decimal/hex parsing: support decimal; hex could be added
  unsigned long long v = 0;
  try {
    v = std::stoull(value);
  } catch (...) {
    // fallback to 0 on parse error
    v = 0;
  }
  std::vector<uint8_t> result(32, 0);
  for (int i = 31; i >= 0; --i) {
    result[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
  return result;
}

std::vector<uint8_t> ABIEncoder::encodeBool(bool v) {
  std::vector<uint8_t> result(32, 0);
  result[31] = v ? 1 : 0;
  return result;
}

std::vector<uint8_t> ABIEncoder::encodeCall(
  const std::vector<uint8_t> &selector,
  const std::vector<std::vector<uint8_t>> &args)
{
  std::vector<uint8_t> out;
  out.insert(out.end(), selector.begin(), selector.end());
  for (const auto& a : args) {
    out.insert(out.end(), a.begin(), a.end());
  }
  return out;
}

} // namespace crypto
