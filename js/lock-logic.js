const HUB_ADDRESS = "0x..."; // Paste your ServiceHub Address here
const LOCKER_ADDRESS = "0x..."; // Paste your ProfessionalLocker Address here
const FLAT_FEE_USD = 45;

async function executeLock(type) {
    if (!window.ethereum) return alert("Please install MetaMask");
    const provider = new ethers.providers.Web3Provider(window.ethereum);
    await provider.send("eth_requestAccounts", []);
    const signer = provider.getSigner();

    // Interaction with ServiceHub for the $45 payment
    const serviceHub = new ethers.Contract(HUB_ADDRESS, [
        "function getQuote(uint256 usdAmount) public view returns (uint256)",
        "function processLock(address locker, address token, uint256 amount, uint256 unlockTime) public payable"
    ], signer);

    const feeInWei = await serviceHub.getQuote(FLAT_FEE_USD);

    try {
        const tx = await serviceHub.processLock(
            LOCKER_ADDRESS,
            document.getElementById('tokenAddress').value,
            ethers.utils.parseUnits(document.getElementById('amount')?.value || "1", 18),
            Math.floor(new Date(document.getElementById('unlockDate').value).getTime() / 1000),
            { value: feeInWei }
        );
        alert(`Transaction Sent! Locking ${type}...`);
        await tx.wait();
        alert("Success!");
    } catch (e) {
        console.error(e);
        alert("Error: " + e.message);
    }
}
