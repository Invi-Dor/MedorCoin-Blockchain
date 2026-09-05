// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

interface IERC20 {
    function transferFrom(address sender, address recipient, uint256 amount) external returns (bool);
    function transfer(address recipient, uint256 amount) external returns (bool);
    function balanceOf(address account) external view returns (uint256);
}

library SafeERC20 {
    function safeTransfer(IERC20 token, address to, uint256 value) internal {
        (bool success, bytes memory data) = address(token).call(abi.encodeWithSelector(token.transfer.selector, to, value));
        require(success && (data.length == 0 || abi.decode(data, (bool))), "SafeERC20: transfer failed");
    }

    function safeTransferFrom(IERC20 token, address from, address to, uint256 value) internal {
        (bool success, bytes memory data) = address(token).call(abi.encodeWithSelector(token.transferFrom.selector, from, to, value));
        require(success && (data.length == 0 || abi.decode(data, (bool))), "SafeERC20: transferFrom failed");
    }
}

contract ReentrancyGuard {
    uint256 private constant _NOT_ENTERED = 1;
    uint256 private constant _ENTERED = 2;
    uint256 private _status = _NOT_ENTERED;
    modifier nonReentrant() {
        require(_status != _ENTERED, "ReentrancyGuard: reentrant call");
        _status = _ENTERED;
        _;
        _status = _NOT_ENTERED;
    }
}

contract Ownable {
    address public owner;
    constructor() { owner = msg.sender; }
    modifier onlyOwner() { require(msg.sender == owner, "Not owner"); _; }
}

contract MedorCoinProfessionalLocker is ReentrancyGuard, Ownable {
    using SafeERC20 for IERC20;

    struct LockInfo {
        uint256 amount;
        uint256 unlockTime;
        bool isWithdrawn;
    }

    mapping(address => LockInfo[]) public userLocks;

    event LiquidityLocked(address indexed user, uint256 amount, uint256 unlockTime);
    event LiquidityWithdrawn(address indexed user, uint256 amount);

    function createLock(address token, uint256 amount, uint256 duration) external nonReentrant {
        require(amount > 0, "Amount must be > 0");
        uint256 unlockTime = block.timestamp + duration;
        
        // CHECKS & EFFECTS FIRST
        userLocks[msg.sender].push(LockInfo({
            amount: amount,
            unlockTime: unlockTime,
            isWithdrawn: false
        }));
        
        emit LiquidityLocked(msg.sender, amount, unlockTime);

        // INTERACTION LAST
        IERC20(token).safeTransferFrom(msg.sender, address(this), amount);
    }

    function createBatchLocks(address token, address[] calldata users, uint256[] calldata amounts, uint256 duration) external nonReentrant {
        require(users.length == amounts.length, "Length mismatch");
        uint256 unlockTime = block.timestamp + duration;
        uint256 totalAmount = 0;

        // EFFECTS FIRST: Process all state variables locally first
        for (uint256 i = 0; i < users.length; i++) {
            require(amounts[i] > 0, "Amount must be > 0");
            userLocks[users[i]].push(LockInfo({
                amount: amounts[i],
                unlockTime: unlockTime,
                isWithdrawn: false
            }));
            totalAmount += amounts[i];
            emit LiquidityLocked(users[i], amounts[i], unlockTime);
        }

        // INTERACTION LAST: Move tokens in one singular safe transfer
        if (totalAmount > 0) {
            IERC20(token).safeTransferFrom(msg.sender, address(this), totalAmount);
        }
    }

    function withdrawLiquidity(address token, uint256 lockIndex) external nonReentrant {
        require(lockIndex < userLocks[msg.sender].length, "Invalid index");
        LockInfo storage lock = userLocks[msg.sender][lockIndex];
        require(block.timestamp >= lock.unlockTime, "Liquidity still locked");
        require(!lock.isWithdrawn, "Already withdrawn");

        uint256 withdrawAmount = lock.amount;
        
        // CHECKS-EFFECTS-INTERACTIONS COMPLETE FIX
        lock.isWithdrawn = true;
        lock.amount = 0;

        emit LiquidityWithdrawn(msg.sender, withdrawAmount);

        IERC20(token).safeTransfer(msg.sender, withdrawAmount);
    }
}
