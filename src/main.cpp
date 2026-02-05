#include "blockchain.h"
#include "transaction.h"
#include "net/net_manager.h"

#include <iostream>
#include <vector>
#include <string>

int main() {
    // Create a blockchain instance
    Blockchain medorChain;

    // NetworkManager with a dummy listen address
    NetworkManager net("127.0.0.1:4001");

    if (!net.start()) {
        std::cerr << "Network failed\n";
        return -1;
    }

    // Dummy bootstrap addresses
    std::vector<std::string> bootstrap = {
        "127.0.0.1:4002",
        "127.0.0.1:4003"
    };

    net.connectBootstrap(bootstrap);

    std::cout << "MedorCoin node started and connected\n";

    // Example broadcast calls
    net.broadcastBlock("01020304deadbeef");
    net.broadcastTx("af23be4c");

    return 0;
}
