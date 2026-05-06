import { ethers } from "ethers";

// Use the ABI from your compiled MedorCoinServiceHub.sol
const CONTRACT_ABI = [...]; 
const CONTRACT_ADDRESS = "YOUR_DEPLOYED_CONTRACT_ADDRESS";

export const medorLocker = {
    // Connect to Metamask
    async connectWallet() {
        if (!window.ethereum) throw new Error("Please install MetaMask");
        const provider = new ethers.providers.Web3Provider(window.ethereum);
        await provider.send("eth_requestAccounts", []);
        return provider.getSigner();
    },

    // 1. Create a Token/LP/NFT Lock
    async createLock(tokenAddress, amountOrId, unlockTimestamp, isNFT) {
        const signer = await this.connectWallet();
        const contract = new ethers.Contract(CONTRACT_ADDRESS, CONTRACT_ABI, signer);
        
        // Fetch current flatFee from contract
        const fee = await contract.flatFee();

        // Standard Token (ERC20) Approval check
        if (!isNFT) {
            const tokenContract = new ethers.Contract(tokenAddress, ["function approve(address,uint256) public returns (bool)"], signer);
            const approveTx = await tokenContract.approve(CONTRACT_ADDRESS, amountOrId);
            await approveTx.wait();
        }

        // Call createLock function from your .sol file
        const tx = await contract.createLock(
            tokenAddress, 
            amountOrId, 
            unlockTimestamp, 
            isNFT, 
            { value: fee }
        );
        return await tx.wait();
    },

    // 2. Withdraw Lock
    async withdraw(lockId) {
        const signer = await this.connectWallet();
        const contract = new ethers.Contract(CONTRACT_ADDRESS, CONTRACT_ABI, signer);
        const tx = await contract.withdraw(lockId);
        return await tx.wait();
    },

    // 3. MultiSend (Airdrop/Payroll)
    async multiSend(tokenAddress, recipients, amounts) {
        const signer = await this.connectWallet();
        const contract = new ethers.Contract(CONTRACT_ADDRESS, CONTRACT_ABI, signer);
        const fee = await contract.flatFee();

        const tx = await contract.multiSend(tokenAddress, recipients, amounts, { value: fee });
        return await tx.wait();
    }
};
