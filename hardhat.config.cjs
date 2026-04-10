require("@nomicfoundation/hardhat-ethers");
require("@nomicfoundation/hardhat-verify");
require("@nomicfoundation/hardhat-toolbox");
require("dotenv").config();

// Your Alchemy RPC URL
const MAINNET_RPC = "https://bob-mainnet.g.alchemy.com/v2/kqKxHB6MaANuxIgI0ALdf"; 

// Using variables from your .env file
const SEPOLIA_RPC = process.env.SEPOLIA_RPC_URL || "https://alchemy.com";
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
    sepolia: {
      url: SEPOLIA_RPC,
      accounts: [PRIVATE_KEY],
      chainId: 11155111,
      timeout: 60000,
    },
    bob: {
      url: MAINNET_RPC,
      accounts: [PRIVATE_KEY],
      chainId: 60808, 
      gas: "auto",
      gasPrice: "auto",
      timeout: 120000,
    },
  },
  etherscan: {
    apiKey: process.env.ETHERSCAN_API_KEY || "",
    customChains: [
      {
        network: "bob",
        chainId: 60808,
        urls: {
          apiURL: "https://explorer.gobob.xyz/api",
          browserURL: "https://explorer.gobob.xyz"
        }
      }
    ]
  },
};
