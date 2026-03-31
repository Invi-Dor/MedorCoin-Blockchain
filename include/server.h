/**
 * File: server.h
 * MEDORCOIN CORE API & GATEWAY DEFINITIONS
 * Purpose: Provides high-speed, multithreaded REST interfaces for mining and transactions.
 */

#ifndef MEDORCOIN_SERVER_H
#define MEDORCOIN_SERVER_H

#include <string>
#include <vector>
#include "crow.h" // Crow framework for high-concurrency handling

/**
 * START API SERVER
 * Initializes the Crow application, opens the RocksDB receipt store,
 * and begins listening for requests on the industrial port (18080).
 */
void startAPIServer();

/**
 * SECURITY: JWT VALIDATION
 * Cryptographically verifies the HS256 signature and expiration.
 * @param token The raw JWT string from the Authorization header.
 * @param userId Output parameter to store the validated User ID / Email.
 * @return True if the session is active and untampered.
 */
bool verifyJWT(const std::string& token, std::string& userId);

/**
 * SECURITY: API KEY GATE
 * Validates the X-API-KEY header for administrative endpoints.
 * @param req The incoming Crow request object.
 * @param res The Crow response object used to send 403 Forbidden on failure.
 * @return True if the key matches the MEDOR_ADMIN_API_KEY environment variable.
 */
bool checkApiKey(const crow::request& req, crow::response& res);

/**
 * UTILITY: STANDARDIZED JSON ERROR
 * Ensures all API errors follow the MedorCoin JSON schema.
 * @param res The response object to modify.
 * @param code The HTTP status code (e.g., 401, 404, 500).
 * @param message The human-readable error description.
 */
void json_error(crow::response& res, int code, const std::string& message);

#endif // MEDORCOIN_SERVER_H
