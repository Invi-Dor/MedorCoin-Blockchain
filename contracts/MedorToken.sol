// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "@openzeppelin/contracts/token/ERC20/ERC20.sol";
import "@openzeppelin/contracts/security/ReentrancyGuard.sol";

/**
 * @title MedorToken (MEDOR)
 * @notice Fixed Supply: 50M | PoW Mining enabled for the 30M public pool.
 */
contract MedorToken is ERC20, ReentrancyGuard {
    
    uint256 public difficulty = 4; // Matches your C++ minDifficulty
    uint256 public constant MINING_REWARD = 50 * 10**18; // 50 MEDOR per block
    
    // Tracking nonces to prevent double-claiming the same work
    mapping(uint64 => bool) public usedNonces;

    constructor(address initialReceiver) ERC20("Medor Token", "MEDOR") {
        // 20M to your MetaMask
        _mint(initialReceiver, 20_000_000 * 10**18);
        
        // 30M held by contract to be distributed via 'claimMiningReward'
        _mint(address(this), 30_000_000 * 10**18);
    }

    /**
     * @dev Validates PoW from your C++ Miner and releases 50 MEDOR.
     * @param nonce The nonce found by your C++ ProofOfWork::mine
     */
    function claimMiningReward(uint64 nonce) external nonReentrant {
        require(!usedNonces[nonce], "Nonce already used");
        
        // Reproduce the Keccak256 hash logic from your C++ serializeHeader
        bytes32 hash = keccak256(abi.encodePacked(msg.sender, nonce));
        
        // Check if hash meets target (Difficulty 4 = 4 leading zeros)
        // This matches your C++ hashMeetsTarget logic
        for (uint i = 0; i < difficulty; i++) {
            require(uint8(hash[i/2] >> (4 * (1 - (i % 2)))) & 0x0f == 0, "Hash does not meet target");
        }

        usedNonces[nonce] = true;
        
        // Transfer 50 MEDOR from the contract's 30M pool to the miner
        uint256 contractBalance = balanceOf(address(this));
        require(contractBalance >= MINING_REWARD, "Mining pool exhausted");
        
        _transfer(address(this), msg.sender, MINING_REWARD);
    }

    // No Owner, No Proxy, No Minting after deployment.
}
