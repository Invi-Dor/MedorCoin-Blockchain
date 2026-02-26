#ifndef CRYPTO_H
#define CRYPTO_H

#include <string>

std::string doubleSHA256(const std::string& input);
std::string generatePrivateKey();
std::string getPublicKey(const std::string& privateKey);
std::string signMessage(const std::string& message, const std::string& privateKey);
bool verifySignature(const std::string& message, const std::string& signature, const std::string& pubKey);

#endif
