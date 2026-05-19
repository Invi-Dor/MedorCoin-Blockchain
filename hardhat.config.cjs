require("@nomicfoundation/hardhat-ethers");
require("@nomicfoundation/hardhat-verify");
require("@nomicfoundation/hardhat-toolbox");
require("dotenv").config();

// Pointing directly to your own standalone network node
const NODE_RPC = process.env.LOCAL_NODE_RPC_URL || "http://127.0.0.1:8545"; 
const PRIVATE_KEY = process.env.MAINNET_PRIVATE_KEY || "0x0000000000000000000000000000000000000000000000000000000000000000";

module.exports = {
  solidity: {
    compilers: [
      {
        version: "0.8.28",
        settings: {
          optimizer: { enabled: true, runs: 1000 },
        },
      },
    ],
  },
  paths: {
    sources: "./", 
    tests: "./test",
    cache: "./cache",
    artifacts: "./artifacts"
  },
  networks: {
    medorcoin: {
      url: NODE_RPC,
      accounts: [PRIVATE_KEY],
      chainId: 60808, 
      gas: "auto",
      gasPrice: "auto",
      timeout: 120000,
    },
  },
};
