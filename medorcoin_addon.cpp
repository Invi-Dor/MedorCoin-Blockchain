include <napi.h>
include "block_builder.h"
include "blockchain.h"
include "utxo.h"

include <limits>
include <string>
include <vector>

static Blockchain chain;
static Mempool mempool(chain);
static BlockBuilder builder(chain, mempool);
static UTXOSet utxoSet;

// Helper: throw a JS error
static void throwTypeError(const Napi::Env& env, const std::string& msg) {
    Napi::TypeError::New(env, msg).ThrowAsJavaScriptException();
}

// Expose getBalance(address)
Napi::Value GetBalance(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        throwTypeError(env, "getBalance requires a string 'address' argument");
        return env.Null();
    }

    std::string address = info[0].As<Napi::String>().Utf8Value();
    uint64_t bal = chain.getBalance(address);
    return Napi::Number::New(env, static_cast<double>(bal));
}

// Expose submitTransaction
Napi::Value SubmitTransaction(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsObject()) {
        throwTypeError(env, "submitTransaction requires a TX object");
        return env.Null();
    }

    Napi::Object txObj = info[0].As<Napi::Object>();

    // Basic required fields
    if (!txObj.Has("from") || !txObj.Has("to") || !txObj.Has("value")) {
        throwTypeError(env, "TX object must include 'from', 'to', and 'value'");
        return env.Null();
    }

    Transaction tx;
    if (txObj.Get("from").IsString())
        tx.fromAddress = txObj.Get("from").As<Napi::String>().Utf8Value();

    if (txObj.Get("to").IsString())
        tx.toAddress = txObj.Get("to").As<Napi::String>().Utf8Value();

    // 64-bit numeric fields (fallback to 0 if missing)
    auto getU64 = & -> uint64_t {
        if (txObj.Has(key) && txObj.Get(key).IsNumber()) {
            double v = txObj.Get(key).As<Napi::Number>().DoubleValue();
            if (v < 0) return 0;
            if (v > static_cast<double>(std::numeric_limits<uint64_t>::max()))
                return std::numeric_limits<uint64_t>::max();
            return static_cast<uint64_t>(v);
        }
        return 0;
    };

    tx.value        = getU64("value");
    tx.gasLimit     = getU64("gasLimit");
    tx.maxFeePerGas = getU64("maxFeePerGas");

    // Optional: txHash (not strictly required for processing here)
    if (txObj.Has("txHash") && txObj.Get("txHash").IsString()) {
        tx.txHash = txObj.Get("txHash").As<Napi::String>().Utf8Value();
    }

    // 2% fee policy on value
    // Use value/50 to avoid overflow
    uint64_t originalValue = tx.value;
    uint64_t fee = (originalValue / 50); // 2%
    if (fee > originalValue) fee = originalValue;

    if (originalValue < fee) {
        throwTypeError(env, "Invalid TX value for computed fee");
        return env.Null();
    }

    tx.value = originalValue - fee;

    // Execute via core (MINER_ADDRESS placeholder)
    const std::string minerAddress = "0x85708d61FEfcbb6eb13C72b0D42bCeB246F06dd0";

    bool success = chain.executeTransaction(tx, minerAddress);

    return Napi::Boolean::New(env, success);
}

// Expose buildBlock
Napi::Value BuildBlock(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        throwTypeError(env, "buildBlock requires a string 'miner' address");
        return env.Null();
    }

    std::string miner = info[0].As<Napi::String>().Utf8Value();
    auto included = builder.buildBlock(miner);

    Napi::Array arr = Napi::Array::New(env, static_cast<int>(included.size()));
    for (size_t i = 0; i < included.size(); ++i) {
        arr.Set(static_cast<uint32_t>(i), Napi::String::New(env, included[i]));
    }

    return arr;
}

// Initialize module
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("getBalance", Napi::Function::New(env, GetBalance));
    exports.Set("submitTransaction", Napi::Function::New(env, SubmitTransaction));
    exports.Set("buildBlock", Napi::Function::New(env, BuildBlock));
    return exports;
}

NODE_API_MODULE(medorcoin_addon, Init)
