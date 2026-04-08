// Replace this line in your config:
const MAINNET_RPC = "https://bob-mainnet.g.alchemy.com/v2/kqKxHB6MaANuxIgI0ALdf"; 
import "@nomicfoundation/hardhat-ethers";
import "@nomicfoundation/hardhat-verify";

const SEPOLIA_RPC = configVariable("SEPOLIA_RPC_URL", "https://alchemy.com");
const MAINNET_RPC = configVariable("MAINNET_RPC_URL", "https://alchemy.com");
const PRIVATE_KEY = configVariable("MAINNET_PRIVATE_KEY", "0x0000000000000000000000000000000000000000000000000000000000000000");

export default defineConfig({
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
    sources: "./contracts",
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
    mainnet: {
      url: MAINNET_RPC,
      accounts: [PRIVATE_KEY],
      chainId: 60808, // BOB Mainnet
      gas: "auto",
      gasPrice: "auto",
      timeout: 120000,
    },
  },
  etherscan: {
    apiKey: configVariable("ETHERSCAN_API_KEY", ""),
    customChains: [
      {
        network: "mainnet",
        chainId: 60808,
        urls: {
          apiURL: "https://gobob.xyz",
          browserURL: "https://gobob.xyz"
        }
      }
    ]
  },
});
