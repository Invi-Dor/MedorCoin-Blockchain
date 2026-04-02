#include <napi.h>
#include <string>
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

private:
    std::string txData;
};

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
    return exports;
}

NODE_API_MODULE(medorcoin_core, Init)
