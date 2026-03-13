#include "crow.h"
#include "transaction.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>
#include "crow.h"
#include "transaction.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <cstdlib>
#include <iostream>
#include "receipts_store.h"
#include <memory>
#include <cstdlib>
#include <unistd.h>
 
// RocksDB-backed receipts store (production)
static ReceiptsStore* g_receiptsStore = nullptr;
 
 static json buildReceipt(const Transaction &tx,
                          const std::string &productName,
                          const std::string &level,
                          double price,
                          const std::string &buyerName) {
@@
 void startAPIServer() {
     crow::SimpleApp app;
     // Initialize RocksDB-backed storage for receipts
     const char* pathEnv = std::getenv("RECEIPTS_ROCKSDB_PATH");
     std::string dbPath = pathEnv ? pathEnv : "./receipts.rocksdb";
     g_receiptsStore = new ReceiptsStore(dbPath);

     // GET /api/transaction/receipt?buyer=...&product=...&level=...&price=...
     // GET /api/transaction/receipt?buyer=...&product=...&level=...&price=...
     CROW_ROUTE(app, "/api/transaction/receipt")
     (&{
         if (!checkApiKey(req, res)) return;
 

         // Build receipt
         // Build receipt
         json receipt = buildReceipt(tx, product, level, price, buyer);
         json receipt = buildReceipt(tx, product, level, price, buyer);
 
         // Persist receipt to RocksDB for later retrieval via /api/receipt/:txHash
         if (g_receiptsStore && receipt.contains("transaction")) {
             const json& t = receipt["transaction"];
             std::string txHash = t.value("txHash", "");
             if (!txHash.empty()) {
                 g_receiptsStore->put(txHash, receipt);
             }
         }
 
         res.code = 200;
         res.code = 200;
         res.set_header("Content-Type", "application/json");
         res.write(receipt.dump(4));
         res.end();
     });
 
     // New: retrieve a receipt by txHash from RocksDB
     CROW_ROUTE(app, "/api/receipt/<string>")(&{
         (void)req; // unused
         if (!g_receiptsStore) {
             json_error(res, 500, "Receipt store not initialized");
             return;
         }
         json value;
         if (g_receiptsStore->get(txHash, value)) {
             res.code = 200;
             res.set_header("Content-Type", "application/json");
             res.write(value.dump(4));
             res.end();
             return;
         } else {
             json_error(res, 404, "Receipt not found for txHash");
         }
     });
 
     app.port(18080).multithreaded().run();
 }
