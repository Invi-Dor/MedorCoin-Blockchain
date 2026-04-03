#include <napi.h>
#include <string>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

// --- Existing Logic Mock ---
namespace MedorCrypto { bool verify(const std::string& d) { return true; } }
namespace Mempool { void push(const std::string& d) {} }

// --- 1. NEW: Mining Verification Logic ---
Napi::Boolean VerifyPoW(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    // Expects (lastHash, nonce, address)
    std::string lastHash = info[0].As<Napi::String>();
    std::string nonce = info[1].As<Napi::String>();
    std::string address = info[2].As<Napi::String>();
    std::string target = "0000"; 

    // Reconstruct data: lastHash + nonce + address
    std::string data = lastHash + nonce + address;

    // Calculate SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)data.c_str(), data.length(), hash);

    // Convert to Hex String
    std::stringstream ss;
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    std::string resultHash = ss.str();

    // Check against target
    bool success = resultHash.substr(0, target.length()) == target;
    return Napi::Boolean::New(env, success);
}

// --- 2. Existing Transaction Worker ---
class MempoolWorker : public Napi::AsyncWorker {
public:
    MempoolWorker(Napi::Function& callback, std::string txData)
        : Napi::AsyncWorker(callback), txData(std::move(txData)) {}

    void Execute() override {
        if (MedorCrypto::verify(txData)) {
            Mempool::push(txData); 
        } else {
            SetError("Invalid Transaction");
        }
    }

    void OnOK() override {
        Callback().Call({Env().Null(), Napi::Boolean::New(Env(), true)});
    }

private:
    std::string txData;
};

Napi::Value SubmitTransaction(const Napi::CallbackInfo& info) {
    std::string data = info[0].As<Napi::String>();
    Napi::Function cb = info[1].As<Napi::Function>();
    (new MempoolWorker(cb, data))->Queue(); 
    return info.Env().Undefined();
}

// --- 3. Combined Initialization ---
Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("submitTransaction", Napi::Function::New(env, SubmitTransaction));
    exports.Set("verifyPoW", Napi::Function::New(env, VerifyPoW));
    return exports;
}

NODE_API_MODULE(medorcoin_core, Init)
