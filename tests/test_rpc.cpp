#include <gtest/gtest.h>
#include "rpc_server.h"
#include "blockchain.h"
#include "utxo.h"
#include "net/serialization.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// =============================================================================
// HELPERS
// =============================================================================

static Blockchain::Config rpcTestConfig(const std::string &suffix = "")
{
    Blockchain::Config cfg;
    cfg.ownerAddress   = "medor_owner";
    cfg.blockDBPath    = "/tmp/test_rpc_blockdb"   + suffix;
    cfg.accountDBPath  = "/tmp/test_rpc_accountdb" + suffix;
    cfg.maxSupply      = 1000000ULL;
    cfg.initialMedor   = 1;
    cfg.initialBaseFee = 1ULL;
    cfg.rewardSchedule = { { UINT64_MAX, 55ULL } };
    return cfg;
}

static RpcRequest makeReq(const std::string &method,
                           const json        &params = json::array(),
                           const json        &id     = 1)
{
    RpcRequest req;
    req.method = method;
    req.params = params;
    req.id     = id;
    return req;
}

static Transaction makeTx(uint64_t nonce   = 1,
                           uint64_t value   = 100,
                           const std::string &to = "recipient")
{
    Transaction tx;
    tx.chainId              = 1;
    tx.nonce                = nonce;
    tx.value                = value;
    tx.toAddress            = to;
    tx.gasLimit             = 21000;
    tx.maxFeePerGas         = 10;
    tx.maxPriorityFeePerGas = 2;
    TxOutput out;
    out.value   = value;
    out.address = to;
    tx.outputs.push_back(out);
    tx.calculateHash();
    return tx;
}

// =============================================================================
// FIXTURE
// =============================================================================

class RpcTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static std::atomic<int> counter{0};
        suffix_ = std::to_string(++counter);
        bc   = std::make_unique<Blockchain>(rpcTestConfig(suffix_));
        utxo = std::make_unique<UTXOSet>();
        srv  = std::make_unique<RpcServer>(8545);
        registerHandlers();
    }

    void TearDown() override
    {
        srv.reset();
        bc.reset();
        utxo.reset();
        std::filesystem::remove_all("/tmp/test_rpc_blockdb"   + suffix_);
        std::filesystem::remove_all("/tmp/test_rpc_accountdb" + suffix_);
    }

    void registerHandlers()
    {
        srv->on("eth_blockNumber",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                r.result = bc->height();
                return r;
            });

        srv->on("eth_getBalance",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                if (!req.params.is_array() || req.params.empty()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_getBalance: missing address param"} };
                    return r;
                }
                if (!req.params[0].is_string()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_getBalance: address must be string"} };
                    return r;
                }
                std::string addr = req.params[0].get<std::string>();
                r.result = bc->getBalance(addr);
                return r;
            });

        srv->on("eth_gasPrice",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                r.result = bc->baseFee();
                return r;
            });

        srv->on("medor_totalSupply",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                r.result = bc->totalSupply();
                return r;
            });

        srv->on("eth_getBlockByNumber",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                auto latest = bc->getLatestBlock();
                if (!latest.has_value()) {
                    r.result = nullptr; return r;
                }
                r.result = serializeBlock(*latest);
                return r;
            });

        srv->on("eth_sendRawTransaction",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                if (!req.params.is_array() || req.params.empty()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_sendRawTransaction: missing params"} };
                    return r;
                }
                if (!req.params[0].is_string()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_sendRawTransaction: param must be string"} };
                    return r;
                }
                try {
                    std::string raw = req.params[0].get<std::string>();
                    json parsed = json::parse(raw);
                    if (!parsed.is_object()) {
                        r.error = { {"code", -32602},
                                    {"message",
                                     "eth_sendRawTransaction: not a JSON object"} };
                        return r;
                    }
                    Transaction tx = deserializeTx(parsed);
                    if (tx.outputs.empty()) {
                        r.error = { {"code", -32602},
                                    {"message",
                                     "eth_sendRawTransaction: tx has no outputs"} };
                        return r;
                    }
                    std::string miner = tx.outputs.front().address;
                    if (miner.empty()) {
                        r.error = { {"code", -32602},
                                    {"message",
                                     "eth_sendRawTransaction: empty miner address"} };
                        return r;
                    }
                    bool ok = bc->addBlock(miner, { tx });
                    if (!ok) {
                        r.error = { {"code", -32000},
                                    {"message",
                                     "eth_sendRawTransaction: tx rejected by chain"} };
                    } else {
                        r.result = tx.txHash;
                    }
                } catch (const SerializationError &e) {
                    r.error = { {"code", -32602},
                                {"message",
                                 std::string("eth_sendRawTransaction: "
                                             "deserialization failed: ")
                                 + e.what()} };
                } catch (const std::exception &e) {
                    r.error = { {"code", -32603},
                                {"message",
                                 std::string("eth_sendRawTransaction: ")
                                 + e.what()} };
                }
                return r;
            });

        srv->on("eth_getUTXOs",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                if (!req.params.is_array() || req.params.empty()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_getUTXOs: missing address param"} };
                    return r;
                }
                if (!req.params[0].is_string()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_getUTXOs: address must be string"} };
                    return r;
                }
                std::string addr = req.params[0].get<std::string>();
                auto utxos = utxo->getUTXOsForAddress(addr);
                json arr = json::array();
                for (const auto &u : utxos) {
                    json item;
                    item["txHash"]      = u.txHash;
                    item["outputIndex"] = u.outputIndex;
                    item["value"]       = u.value;
                    item["address"]     = u.address;
                    item["blockHeight"] = u.blockHeight;
                    item["isCoinbase"]  = u.isCoinbase;
                    arr.push_back(item);
                }
                r.result = arr;
                return r;
            });

        srv->on("eth_getUTXOBalance",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                if (!req.params.is_array() || req.params.empty()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_getUTXOBalance: missing address param"} };
                    return r;
                }
                if (!req.params[0].is_string()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "eth_getUTXOBalance: address must be string"} };
                    return r;
                }
                std::string addr = req.params[0].get<std::string>();
                auto bal = utxo->getBalance(addr);
                r.result = bal.value_or(0ULL);
                return r;
            });

        srv->on("medor_validateChain",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                auto res = bc->validateChain();
                json out;
                out["ok"]     = res.ok;
                out["reason"] = res.reason;
                r.result = out;
                return r;
            });

        srv->on("medor_hasBlock",
            [&](const RpcRequest &req) -> RpcResponse {
                RpcResponse r; r.id = req.id;
                if (!req.params.is_array() || req.params.empty()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "medor_hasBlock: missing hash param"} };
                    return r;
                }
                if (!req.params[0].is_string()) {
                    r.error = { {"code", -32602},
                                {"message",
                                 "medor_hasBlock: hash must be string"} };
                    return r;
                }
                std::string hash = req.params[0].get<std::string>();
                r.result = bc->hasBlock(hash);
                return r;
            });

        srv->on("method_throws",
            [&](const RpcRequest &req) -> RpcResponse {
                throw std::runtime_error("handler crash");
            });
    }

    json dispatch(const RpcRequest &req)
    {
        return json::parse(srv->dispatchForTest(req).toJson());
    }

    std::unique_ptr<Blockchain> bc;
    std::unique_ptr<UTXOSet>    utxo;
    std::unique_ptr<RpcServer>  srv;
    std::string                 suffix_;
};

// =============================================================================
// RPC RESPONSE FORMAT
// =============================================================================

TEST_F(RpcTest, ResponseContainsJsonrpc20) {
    RpcResponse r; r.id = 1; r.result = "ok";
    EXPECT_EQ(json::parse(r.toJson())["jsonrpc"], "2.0");
}

TEST_F(RpcTest, ResponseContainsId) {
    RpcResponse r; r.id = 42; r.result = "v";
    EXPECT_EQ(json::parse(r.toJson())["id"], 42);
}

TEST_F(RpcTest, SuccessResponseContainsResult) {
    RpcResponse r; r.id = 1; r.result = "ok";
    auto j = json::parse(r.toJson());
    EXPECT_TRUE(j.contains("result"));
    EXPECT_FALSE(j.contains("error"));
}

TEST_F(RpcTest, ErrorResponseContainsError) {
    RpcResponse r; r.id = 1;
    r.error = { {"code", -32601}, {"message", "Not found"} };
    auto j = json::parse(r.toJson());
    EXPECT_TRUE(j.contains("error"));
    EXPECT_FALSE(j.contains("result"));
}

TEST_F(RpcTest, ErrorResponseCodeIsCorrect) {
    RpcResponse r; r.id = 1;
    r.error = { {"code", -32601}, {"message", "x"} };
    EXPECT_EQ(json::parse(r.toJson())["error"]["code"], -32601);
}

TEST_F(RpcTest, ErrorResponseMessageIsCorrect) {
    RpcResponse r; r.id = 1;
    r.error = { {"code", -32601}, {"message", "Method not found"} };
    EXPECT_EQ(json::parse(r.toJson())["error"]["message"],
              "Method not found");
}

TEST_F(RpcTest, ResponseWithNullResult) {
    RpcResponse r; r.id = 5; r.result = nullptr;
    EXPECT_EQ(json::parse(r.toJson())["id"], 5);
}

TEST_F(RpcTest, ResponseWithIntResult) {
    RpcResponse r; r.id = 1; r.result = 12345;
    EXPECT_EQ(json::parse(r.toJson())["result"], 12345);
}

TEST_F(RpcTest, ResponseWithStringId) {
    RpcResponse r; r.id = "req-001"; r.result = "ok";
    EXPECT_EQ(json::parse(r.toJson())["id"], "req-001");
}

TEST_F(RpcTest, ResponseWithArrayResult) {
    RpcResponse r; r.id = 1;
    r.result = json::array({ 1, 2, 3 });
    auto j = json::parse(r.toJson());
    EXPECT_TRUE(j["result"].is_array());
    EXPECT_EQ(j["result"].size(), 3U);
}

TEST_F(RpcTest, ResponseWithObjectResult) {
    RpcResponse r; r.id = 1;
    r.result = json{ {"key", "value"} };
    EXPECT_EQ(json::parse(r.toJson())["result"]["key"], "value");
}

// =============================================================================
// RPC CALL EXECUTION
// =============================================================================

TEST_F(RpcTest, EthBlockNumberHandlerExecuted) {
    auto j = dispatch(makeReq("eth_blockNumber"));
    EXPECT_TRUE(j.contains("result"));
    EXPECT_GE(j["result"].get<size_t>(), 0ULL);
}

TEST_F(RpcTest, E​​​​​​​​​​​​​​​​
