// medorcoin_addon.cpp
#include <napi.h>
#include "block_builder.h"
#include "blockchain.h"
#include "utxo.h"

static Blockchain chain;
static Mempool mempool(chain);
static BlockBuilder builder(chain, mempool);
static UTXOSet utxoSet;

// Expose getBalance
Napi::Value GetBalance(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::string address = info[0].As<Napi::String>().Utf8Value();
    uint64_t bal = chain.getBalance(address);
    return Napi::Number::New(env, bal);
}

// Expose submitTransaction
Napi::Value SubmitTransaction(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object txObj = info[0].As<Napi::Object>();

    Transaction tx;
    tx.fromAddress = txObj.Get("from").As<Napi::String>().Utf8Value();
    tx.toAddress   = txObj.Get("to").As<Napi::String>().Utf8Value();
    tx.value       = txObj.Get("value").As<Napi::Number>().Uint32Value();
    tx.gasLimit    = txObj.Get("gasLimit").As<Napi::Number>().Uint32Value();
    tx.maxFeePerGas = txObj.Get("maxFeePerGas").As<Napi::Number>().Uint32Value();
    tx.txHash      = txObj.Get("txHash").As<Napi::String>().Utf8Value();

    // Apply 2% fee
    uint64_t fee = (tx.value * 2) / 100;
    tx.value -= fee;

    bool success = chain.executeTransaction(tx, "MINER_ADDRESS");

    return Napi::Boolean::New(env, success);
}

// Expose buildBlock
Napi::Value BuildBlock(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::string miner = info[0].As<Napi::String>().Utf8Value();
    auto included = builder.buildBlock(miner);

    Napi::Array arr = Napi::Array::New(env, included.size());
    for (size_t i = 0; i < included.size(); ++i) {
        arr.Set(i, Napi::String::New(env, included[i]));
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
