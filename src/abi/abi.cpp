#include "abi/abi.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h> // For Keccak256: we'll adapt

using namespace std;

// -- Replace SHA256 with real keccak256 if available --
// For real Abi, use Keccak‑256 (Ethereum); for now we adapt SHA256 for demo
static vector<uint8_t> keccak256(const string &input) {
    vector<uint8_t> out(32);
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), out.data());
    return out;
}

vector<ABIFunction> ABIParser::parseABI(const string &jsonText) {
    vector<ABIFunction> functions;

    json j = json::parse(jsonText);
    for (auto &item : j) {
        if (item["type"] == "function") {
            ABIFunction fn;
            fn.name = item["name"];

            // parse inputs
            for (auto &inp : item["inputs"]) {
                fn.inputs.push_back({ inp.value("name", ""), inp["type"] });
            }
            // parse outputs
            for (auto &out : item["outputs"]) {
                fn.outputs.push_back({ out.value("name", ""), out["type"] });
            }

            // build canonical signature
            ostringstream sig;
            sig << fn.name << "(";
            for (size_t i = 0; i < fn.inputs.size(); ++i) {
                sig << fn.inputs[i].type;
                if (i + 1 < fn.inputs.size()) sig << ",";
            }
            sig << ")";
            fn.signature = sig.str();

            functions.push_back(fn);
        }
    }
    return functions;
}

vector<uint8_t> ABIParser::generateSelector(const string &funcSignature) {
    auto hash = keccak256(funcSignature);
    return vector<uint8_t>(hash.begin(), hash.begin() + 4);
}
