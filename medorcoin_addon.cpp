#include <napi.h>
#include <string>
<<<<<<< HEAD
#include <thread>
#include <queue>
#include <mutex>

// Mocking these for compilation - ensure your headers are linked!
namespace MedorCrypto { bool verify(const std::string& d) { return true; } }
namespace Mempool { void push(const std::string& d) {} }

class MempoolWorker : public Napi::AsyncWorker {
public:
    // Pass the function directly to the base constructor
    MempoolWorker(Napi::Function& callback, std::string txData)
        : Napi::AsyncWorker(callback), txData(std::move(txData)) {}

    ~MempoolWorker() {}

    // Executed on the libuv worker thread (Background)
    void Execute() override {
        try {
            // High-speed C++ Validation
            if (MedorCrypto::verify(txData)) {
                Mempool::push(txData); 
            } else {
                // This triggers OnError instead of OnOK
                SetError("Invalid Transaction Signature or Double-Spend Detected");
            }
        } catch (const std::exception& e) {
            SetError(std::string("Internal Core Error: ") + e.what());
        } catch (...) {
            SetError("Unknown Cryptographic Failure");
        }
    }

    // Executed on the Main JS Thread after Execute() finishes successfully
    void OnOK() override {
        Napi::HandleScope scope(Env());
        // Standard (null, result) pattern
        Callback().Call({Env().Null(), Napi::Boolean::New(Env(), true)});
    }

    // Executed on the Main JS Thread if SetError() was called
    void OnError(const Napi::Error& e) override {
        Napi::HandleScope scope(Env());
        Callback().Call({e.Value(), Env().Undefined()});
    }

=======
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

>>>>>>> 0db22492d1d94bd909f1b5978065f60982ec7c05
private:
    std::string txData;
};

<<<<<<< HEAD
// JS Wrapper: addon.submitTransaction(data, callback)
Napi::Value SubmitTransaction(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // ERROR FIX: Robust Input Validation
    if (info.Length() < 2) {
        Napi::TypeError::New(env, "Wrong number of arguments").ThrowAsJavaScriptException();
        return env.Null();
    }

    if (!info[0].IsString() || !info[1].IsFunction()) {
        Napi::TypeError::New(env, "Invalid arguments: Expected (String, Function)").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string data = info[0].As<Napi::String>();
    Napi::Function cb = info[1].As<Napi::Function>();

    // The 'new' worker is managed and deleted automatically by Napi::AsyncWorker
    MempoolWorker* worker = new MempoolWorker(cb, data);
    worker->Queue(); 
    
    return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "submitTransaction"), 
                Napi::Function::New(env, SubmitTransaction));
=======
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
>>>>>>> 0db22492d1d94bd909f1b5978065f60982ec7c05
    return exports;
}

NODE_API_MODULE(medorcoin_core, Init)
