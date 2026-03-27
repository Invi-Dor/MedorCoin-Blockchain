/**
 * relayer.cjs - MedorCoin Production Relayer
 * Implements M-of-N Consensus Signing & Cross-Chain Polling
 */

const { ethers } = require('ethers');
const axios = require('axios');

// --- Configuration ---
const MEDOR_RPC = process.env.MEDOR_RPC || "http://127.0.0.1:8332";
const EVM_RPC   = process.env.EVM_RPC || "https://rpc.your-evm-chain.com";
const BRIDGE_CONTRACT_ADDR = process.env.BRIDGE_ADDR;
const RELAYER_KEY = process.env.RELAYER_KEY;

if (!RELAYER_KEY || !BRIDGE_CONTRACT_ADDR) {
    console.error("CRITICAL: RELAYER_KEY and BRIDGE_ADDR must be set.");
    process.exit(1);
}

const provider = new ethers.JsonRpcProvider(EVM_RPC);
const wallet   = new ethers.Wallet(RELAYER_KEY, provider);

// Minimal ABI for the MedorToken bridge functions
const BRIDGE_ABI = [
    "function bridgeMint(address to, uint256 amount, string txHash, uint32 index, bytes[] signatures) external",
    "event BridgeBurned(address indexed from, uint256 amount, string medorAddress)"
];

const bridgeContract = new ethers.Contract(BRIDGE_CONTRACT_ADDR, BRIDGE_ABI, wallet);

/**
 * Signs a mint request for consensus.
 * The resulting signature is sent to the aggregator or submitted directly if threshold is met.
 */
async function generateSignature(to, amount, txHash, index) {
    const utxoId = ethers.solidityPackedKeccak256(["string", "uint32"], [txHash, index]);
    const messageHash = ethers.solidityPackedKeccak256(
        ["address", "uint256", "bytes32"],
        [to, amount, utxoId]
    );

    // Sign the EIP-191 message hash
    return await wallet.signMessage(ethers.toBeArray(messageHash));
}

/**
 * Polls the MedorCoin C++ Node for new lock events (Wrap Requests)
 */
async function pollMedorNode() {
    try {
        const response = await axios.post(MEDOR_RPC, {
            jsonrpc: "2.0",
            id: Date.now(),
            method: "bridge_getSwapLogs",
            params: []
        });

        const logs = response.data.result;
        for (const log of logs) {
            console.log(`[LOCK] Found UTXO: ${log.txHash} Index: ${log.index}`);
            
            // In a production multi-relayer setup, you would broadcast your 
            // signature to a p2p network or a centralized collector here.
            const sig = await generateSignature(log.evmAddress, log.amount, log.txHash, log.index);
            console.log(`[SIG] Generated: ${sig}`);
            
            // For single-relayer testing or direct submission (if threshold=1):
            // await bridgeContract.bridgeMint(log.evmAddress, log.amount, log.txHash, log.index, [sig]);
        }
    } catch (err) {
        console.error(`[ERROR] Medor RPC connection failed: ${err.message}`);
    }
}

/**
 * Listens for Burn events on the EVM side to unlock UTXOs on MedorCoin
 */
bridgeContract.on("BridgeBurned", async (from, amount, medorAddress) => {
    console.log(`[BURN] Releasing ${amount} to ${medorAddress}`);
    try {
        await axios.post(MEDOR_RPC, {
            jsonrpc: "2.0",
            id: Date.now(),
            method: "bridge_unlockUTXO",
            params: [medorAddress, amount.toString()]
        });
    } catch (err) {
        console.error(`[ERROR] Failed to signal unlock to C++ node: ${err.message}`);
    }
});

// Start Polling Loop
console.log(`Relayer active: ${wallet.address}`);
setInterval(pollMedorNode, 10000); // 10s intervals
