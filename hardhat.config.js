import { configVariable, defineConfig } from "hardhat/config";
import "@nomicfoundation/hardhat-ethers";
import "@nomicfoundation/hardhat-verify";

// 3. Fallback handling for safety
const SEPOLIA_RPC = configVariable("SEPOLIA_RPC_URL", "https://alchemy.com");
const MAINNET_RPC = configVariable("MAINNET_RPC_URL", "https://alchemy.com");
const PRIVATE_KEY = configVariable("MAINNET_PRIVATE_KEY", "0x0000000000000000000000000000000000000000000000000000000000000000");

export default defineConfig({
  // 4. Future-proof compilers array
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
  // 5. Explicit paths for project structure
  paths: {
    sources: "./contracts",
    tests: "./test",
    cache: "./cache",
    artifacts: "./artifacts"
  },
  networks: {
    // 1 & 2. Network control with gas, timeouts, and chainIds
    sepolia: {
      url: SEPOLIA_RPC,
      accounts: [PRIVATE_KEY],
      chainId: 11155111,
      timeout: 60000, // 60 seconds
    },
    mainnet: {
      url: MAINNET_RPC,
      accounts: [PRIVATE_KEY],
      chainId: 60808, // BOB Mainnet Chain ID
      gas: "auto",
      gasPrice: "auto",
      timeout: 120000, // 120 seconds
    },
  },
  etherscan: {
    apiKey: configVariable("ETHERSCAN_API_KEY", ""),
  },
});
