// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "@openzeppelin/contracts/token/ERC20/ERC20.sol";
// Corrected path for OpenZeppelin v5.x
import "@openzeppelin/contracts/utils/ReentrancyGuard.sol";

/**
 * @title MedorToken (MEDOR)
 * @notice Fixed Supply: 50M | PoW Mining enabled for the 30M public pool.
 */
contract MedorToken is ERC20, ReentrancyGuard {
    
    uint256 public difficulty = 4; 
    uint256 public constant MINING_REWARD = 50 * 10**18; 
    
    mapping(uint64 => bool) public usedNonces;

    constructor(address initialReceiver) ERC20("Medor Token", "MEDOR") {
        // 20M to your wallet
        _mint(initialReceiver, 20_000_000 * 10**18);
        
        // 30M held by contract for miners
        _mint(address(this), 30_000_000 * 10**18);
    }

    function claimMiningReward(uint64 nonce) external nonReentrant {
        require(!usedNonces[nonce], "Nonce already used");
        
        bytes32 hash = keccak256(abi.encodePacked(msg.sender, nonce));
        
        for (uint i = 0; i < difficulty; i++) {
            require(uint8(hash[i/2] >> (4 * (1 - (i % 2)))) & 0x0f == 0, "Hash does not meet target");
        }

        usedNonces[nonce] = true;
        
        uint256 contractBalance = balanceOf(address(this));
        require(contractBalance >= MINING_REWARD, "Mining pool exhausted");
        
        _transfer(address(this), msg.sender, MINING_REWARD);
    }
}
