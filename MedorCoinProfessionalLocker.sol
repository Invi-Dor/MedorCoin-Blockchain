// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import "@openzeppelin/contracts/token/ERC721/IERC721.sol";
import "@openzeppelin/contracts/token/ERC721/IERC721Receiver.sol";
import "@openzeppelin/contracts/security/ReentrancyGuard.sol";
import "@openzeppelin/contracts/access/Ownable.sol";
import "@chainlink/contracts/src/v0.8/interfaces/AggregatorV3Interface.sol";

/**
 * @title MedorCoin Professional Locker
 * @notice Production-grade locker for Tokens and NFTs with dynamic $45 USD fee.
 */
contract MedorCoinProfessionalLocker is ReentrancyGuard, Ownable, IERC721Receiver {
    using SafeERC20 for IERC20;

    struct Lock {
        address owner;
        address token;
        uint256 amountOrId;
        uint256 unlockTime;
        bool isNFT;
        bool active;
    }

    // State Variables
    mapping(uint256 => Lock) public locks;
    mapping(address => uint256[]) private _userLockIds; 
    mapping(address => uint256) public totalLockedPerToken; // Security: Prevents admin rescue of user funds
    
    uint256 public nextLockId;
    AggregatorV3Interface internal priceFeed;
    uint256 public constant USD_FEE = 45; 
    address payable public treasury;

    // Events
    event TokenLocked(uint256 indexed id, address indexed owner, address indexed token, uint256 amount, uint256 unlockTime);
    event Withdrawn(uint256 indexed id, address indexed owner, uint256 amount);
    event EmergencyRescued(address token, uint256 amount);

    constructor(address _priceFeed, address payable _treasury) Ownable() {
        require(_priceFeed != address(0) && _treasury != address(0), "Invalid Config");
        priceFeed = AggregatorV3Interface(_priceFeed);
        treasury = _treasury;
    }

    /**
     * @notice Calculates the native token amount equivalent to $45 USD via Chainlink.
     */
    function getRequiredPayment() public view returns (uint256) {
        (, int256 price, , , ) = priceFeed.latestRoundData();
        uint8 decimals = priceFeed.decimals();
        uint256 adjustedPrice = uint256(price) * (10**(18 - decimals));
        return (USD_FEE * 1e18 * 1e18) / adjustedPrice;
    }

    /**
     * @notice Standard Single Lock with $45 USD fee.
     */
    function createLock(
        address _token, 
        uint256 _amountOrId, 
        uint256 _unlockTime, 
        bool _isNFT
    ) external payable nonReentrant {
        uint256 required = getRequiredPayment();
        require(msg.value >= required, "Insufficient $45 fee");
        
        _executeLock(_token, _amountOrId, _unlockTime, _isNFT);
        
        // Immediate transfer of fee to treasury
        (bool success, ) = treasury.call{value: msg.value}("");
        require(success, "Treasury transfer failed");
    }

    /**
     * @notice Batch Create Locks: Lock multiple assets in one transaction.
     */
    function createBatchLocks(
        address[] calldata _tokens,
        uint256[] calldata _amountsOrIds,
        uint256[] calldata _unlockTimes,
        bool[] calldata _isNFTs
    ) external payable nonReentrant {
        uint256 count = _tokens.length;
        require(count > 0 && count <= 50, "Invalid batch size");
        require(count == _amountsOrIds.length && count == _unlockTimes.length && count == _isNFTs.length, "Array mismatch");

        uint256 totalRequired = getRequiredPayment() * count;
        require(msg.value >= totalRequired, "Insufficient total $45 fees");

        for (uint256 i = 0; i < count; i++) {
            _executeLock(_tokens[i], _amountsOrIds[i], _unlockTimes[i], _isNFTs[i]);
        }

        (bool success, ) = treasury.call{value: msg.value}("");
        require(success, "Treasury transfer failed");
    }

    /**
     * @dev Internal logic for security and gas optimization.
     */
    function _executeLock(address _token, uint256 _amountOrId, uint256 _unlockTime, bool _isNFT) internal {
        require(_unlockTime > block.timestamp, "Unlock must be in future");
        require(_unlockTime < block.timestamp + 3650 days, "Max lock 10 years");

        if (_isNFT) {
            IERC721(_token).safeTransferFrom(msg.sender, address(this), _amountOrId);
        } else {
            uint256 balBefore = IERC20(_token).balanceOf(address(this));
            IERC20(_token).safeTransferFrom(msg.sender, address(this), _amountOrId);
            uint256 actualAmount = IERC20(_token).balanceOf(address(this)) - balBefore;
            totalLockedPerToken[_token] += actualAmount;
            _amountOrId = actualAmount;
        }

        locks[nextLockId] = Lock({
            owner: msg.sender,
            token: _token,
            amountOrId: _amountOrId,
            unlockTime: _unlockTime,
            isNFT: _isNFT,
            active: true
        });

        _userLockIds[msg.sender].push(nextLockId);
        emit TokenLocked(nextLockId, msg.sender, _token, _amountOrId, _unlockTime);
        nextLockId++;
    }

    /**
     * @notice Withdraw tokens after unlock time.
     */
    function withdraw(uint256 _id) external nonReentrant {
        Lock storage l = locks[_id];
        require(msg.sender == l.owner, "Not authorized");
        require(block.timestamp >= l.unlockTime, "Still locked");
        require(l.active, "Inactive");

        l.active = false;
        if (!l.isNFT) totalLockedPerToken[l.token] -= l.amountOrId;

        if (l.isNFT) {
            IERC721(l.token).safeTransferFrom(address(this), msg.sender, l.amountOrId);
        } else {
            IERC20(l.token).safeTransfer(msg.sender, l.amountOrId);
        }
        emit Withdrawn(_id, msg.sender, l.amountOrId);
    }

    /**
     * @notice Enumeration for frontend
     */
    function getUserLocks(address _user) external view returns (uint256[] memory) {
        return _userLockIds[_user];
    }

    /**
     * @notice Rescue tokens accidentally sent to contract (cannot touch user-locked funds).
     */
    function rescueExcessTokens(address _token, uint256 _amount) external onlyOwner {
        uint256 contractBalance = (_token == address(0)) ? address(this).balance : IERC20(_token).balanceOf(address(this));
        uint256 rescuable = contractBalance - totalLockedPerToken[_token];
        require(_amount <= rescuable, "Cannot rescue user funds");

        if (_token == address(0)) {
            payable(owner()).transfer(_amount);
        } else {
            IERC20(_token).safeTransfer(owner(), _amount);
        }
        emit EmergencyRescued(_token, _amount);
    }

    function onERC721Received(address, address, uint256, bytes calldata) external pure override returns (bytes4) {
        return this.onERC721Received.selector;
    }
}
