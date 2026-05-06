// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import "@openzeppelin/contracts/security/ReentrancyGuard.sol";
import "@openzeppelin/contracts/access/Ownable.sol";
import "@chainlink/contracts/src/v0.8/interfaces/AggregatorV3Interface.sol";

/**
 * @title MedorCoin Professional Liquidity Locker
 * @notice Designed to lock DEX LP tokens to prevent liquidity removal (Rug-pull protection).
 */
contract MedorCoinProfessionalLocker is ReentrancyGuard, Ownable {
    using SafeERC20 for IERC20;

    struct Lock {
        address owner;
        address lpToken;      // The DEX Pair Address (e.g. MEDOR/BNB LP)
        uint256 amount;
        uint256 unlockTime;
        bool active;
    }

    mapping(uint256 => Lock) public locks;
    mapping(address => uint256[]) private _userLockIds;
    mapping(address => uint256) public totalLockedLP; // Tracks specific LP token totals

    uint256 public nextLockId;
    AggregatorV3Interface internal priceFeed;
    uint256 public constant USD_FEE = 45; 
    address payable public treasury;

    // EXEMPTIONS
    address public constant ADMIN_WALLET = 0xD81e7078bEE7ad3a313a74ED171E2941b7455f1D;
    mapping(address => bool) public isExemptLP; // Can be used to exempt specific Medor LP pairs

    event LiquidityLocked(uint256 indexed id, address indexed owner, address indexed lpToken, uint256 amount, uint256 unlockTime);
    event LiquidityWithdrawn(uint256 indexed id, address indexed owner, uint256 amount);

    constructor(address _priceFeed, address payable _treasury) Ownable() {
        priceFeed = AggregatorV3Interface(_priceFeed);
        treasury = _treasury;
    }

    /**
     * @notice Set specific LP pairs as fee-exempt (e.g. your main MEDOR/BNB pool)
     */
    function setLPExemption(address _lpToken, bool _status) external onlyOwner {
        isExemptLP[_lpToken] = _status;
    }

    function getRequiredPayment() public view returns (uint256) {
        (, int256 price, , , ) = priceFeed.latestRoundData();
        uint8 decimals = priceFeed.decimals();
        uint256 adjustedPrice = uint256(price) * (10**(18 - decimals));
        return (USD_FEE * 1e18 * 1e18) / adjustedPrice;
    }

    /**
     * @notice Lock LP tokens to secure market liquidity.
     * @param _lpToken The address of the PancakeSwap/Uniswap LP pair.
     * @param _amount The amount of LP tokens to lock.
     * @param _unlockTime Timestamp when liquidity can be removed (e.g. 1 year from now).
     */
    function lockLiquidity(address _lpToken, uint256 _amount, uint256 _unlockTime) external payable nonReentrant {
        require(_unlockTime > block.timestamp, "Unlock must be in future");

        // FEE CHECK: Exempt if sender is Admin OR if the LP token is marked exempt
        if (msg.sender != ADMIN_WALLET && msg.sender != owner() && !isExemptLP[_lpToken]) {
            uint256 required = getRequiredPayment();
            require(msg.value >= required, "Insufficient $45 USD fee");
            (bool success, ) = treasury.call{value: msg.value}("");
            require(success, "Fee transfer failed");
        }

        uint256 balBefore = IERC20(_lpToken).balanceOf(address(this));
        IERC20(_lpToken).safeTransferFrom(msg.sender, address(this), _amount);
        uint256 actualAmount = IERC20(_lpToken).balanceOf(address(this)) - balBefore;

        locks[nextLockId] = Lock({
            owner: msg.sender,
            lpToken: _lpToken,
            amount: actualAmount,
            unlockTime: _unlockTime,
            active: true
        });

        totalLockedLP[_lpToken] += actualAmount;
        _userLockIds[msg.sender].push(nextLockId);
        
        emit LiquidityLocked(nextLockId, msg.sender, _lpToken, actualAmount, _unlockTime);
        nextLockId++;
    }

    /**
     * @notice Withdraw LP tokens ONLY after the lock period has expired.
     */
    function withdrawLiquidity(uint256 _id) external nonReentrant {
        Lock storage l = locks[_id];
        require(msg.sender == l.owner, "Not owner");
        require(block.timestamp >= l.unlockTime, "Liquidity is still locked");
        require(l.active, "Already withdrawn");

        uint256 amount = l.amount;
        l.active = false;
        totalLockedLP[l.lpToken] -= amount;

        IERC20(l.lpToken).safeTransfer(msg.sender, amount);
        emit LiquidityWithdrawn(_id, msg.sender, amount);
    }

    function getUserLocks(address _user) external view returns (uint256[] memory) {
        return _userLockIds[_user];
    }
}
