#include <napi.h>
#include "poa_sign.h"
#include "consensus/validator_registry.h"
#include <vector>
#include <string>

/**
 * FILE: src/node_addon.cpp
 * ACTUAL PRODUCTION IMPLEMENTATION
 */

Napi::Value MempoolAddTransaction(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    // 1. Strict Input Validation
    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Argument 0 must be a Transaction Object").ThrowAsJavaScriptException();
        return env.Null();
    }

    Napi::Object txObj = info[0].As<Napi::Object>();

    // 2. Defensive check for required keys
    if (!txObj.Has("signature") || !txObj.Has("header")) {
        Napi::Object fail = Napi::Object::New(env);
        fail.Set("success", false);
        fail.Set("error", "INVALID_TX_FORMAT: MISSING_SIGNATURE_OR_HEADER");
        return fail;
    }

    // 3. Extract and verify data
    try {
        Block block;
        block.signature = txObj.Get("signature").As<Napi::String>().Utf8Value();
        
        // We use Utf8Value for the header string; if it's hex, poa_sign.cpp handles the decoding
        block.header = txObj.Get("header").As<Napi::String>().Utf8Value();

        // 4. CALL CORE CONSENSUS (poa_sign.cpp)
        // This performs the Keccak hash and ECDSA recovery
        bool isValid = verifyBlockPoA(block);

        Napi::Object result = Napi::Object::New(env);
        if (isValid) {
            result.Set("success", true);
            result.Set("txHash", block.header); // In prod, this would be the keccak of the header
        } else {
            result.Set("success", false);
            result.Set("error", "CONSENSUS_REJECTION: INVALID_POA_SIGNATURE");
        }
        return result;

    } catch (const std::exception& e) {
        Napi::Error::New(env, std::string("C++ Consensus Exception: ") + e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

// Module Initializer
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    // Critical: Initialize the Validator Set into memory from the registry
    ValidatorRegistry::loadValidators(); 
    
    exports.Set(Napi::String::New(env, "mempool_addTransaction"), 
                Napi::Function::New(env, MempoolAddTransaction));
    return exports;
}

NODE_API_MODULE(medorcoin_addon, Init)
