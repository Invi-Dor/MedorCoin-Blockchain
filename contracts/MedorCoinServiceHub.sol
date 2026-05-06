// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/utils/SafeERC20.sol";
import "@openzeppelin/contracts/token/ERC721/IERC721.sol";
import "@openzeppelin/contracts/token/ERC721/IERC721Receiver.sol";
import "@openzeppelin/contracts/utils/introspection/IERC165.sol";
import "@openzeppelin/contracts/security/ReentrancyGuard.sol";
import "@openzeppelin/contracts/security/Pausable.sol";
import "@openzeppelin/contracts/access/Ownable.sol";
import "@openzeppelin/contracts/utils/Address.sol";

contract MedorCoinServiceHub is ReentrancyGuard, Pausable, Ownable, IERC721Receiver {
    using SafeERC20 for IERC20;
    using Address   for address payable;

    // ── Custom errors ─────────────────────────────────────────────────────────

    error InvalidToken();
    error InvalidAddress();
    error InsufficientFee();
    error UnlockInPast();
    error UnlockTooFar();
    error InvalidAmount();
    error ZeroReceived();
    error LockInactive();
    error NotLockOwner();
    error StillLocked();
    error BatchSizeError();
    error ArrayMismatch();
    error ZeroAddressRecipient();
    error ActiveERC20LocksExist();
    error ActiveNFTLocksExist();
    error TokenIsNFTContract();
    error TokenIsNotNFTContract();
    error FeeTooHigh();
    error FeeTransferFailed();
    error NothingToRescue();

    // ── Data structures ───────────────────────────────────────────────────────

    struct Lock {
        address owner;
        address token;
        uint256 amountOrId;
        uint256 startTime;
        uint256 unlockTime;
        bool    isNFT;
        bool    active;
    }

    // ── Constants ─────────────────────────────────────────────────────────────

    uint256 public constant MAX_LOCK_DURATION = 10 * 365 days;
    uint256 public constant MAX_BATCH_SIZE    = 100;
    uint256 public constant MAX_FLAT_FEE      = 1 ether;

    // ── State ─────────────────────────────────────────────────────────────────

    mapping(uint256 => Lock)                        public  locks;
    mapping(address => uint256)                     public  activeTokenLocks;
    mapping(address => uint256)                     public  activeNFTLocks;
    mapping(address => uint256[])                   private _userLockIds;
    mapping(address => mapping(uint256 => uint256)) private _userLockIndex;

    uint256         public nextLockId;
    address payable public treasury;
    uint256         public flatFee;

    // ── Events ────────────────────────────────────────────────────────────────

    event TokenLocked(uint256 indexed lockId, address indexed owner, address token, uint256 amount);
    event Withdrawn(uint256 indexed lockId, address indexed owner, uint256 amount);
    event MultiSent(address indexed token, uint256 totalRecipients, uint256 totalAmount);
    event LockTransferred(uint256 indexed lockId, address indexed oldOwner, address indexed newOwner);
    event ERC20Rescued(address indexed token, address indexed to, uint256 amount);
    event NFTRescued(address indexed token, address indexed to, uint256 tokenId);
    event ETHRescued(address indexed to, uint256 amount);
    event FlatFeeUpdated(uint256 oldFee, uint256 newFee);
    event TreasuryUpdated(address indexed oldTreasury, address indexed newTreasury);

    // ── Constructor ───────────────────────────────────────────────────────────

    constructor(
        uint256 _initialFee
    ) Ownable(msg.sender) {
        if (_initialFee > MAX_FLAT_FEE) revert FeeTooHigh();
        flatFee  = _initialFee;
        treasury = payable(0xD81e7078bEE7ad3a313a74ED171E2941b7455f1D);
    }

    // ── Core: createLock ──────────────────────────────────────────────────────

    function createLock(
        address _token,
        uint256 _amountOrId,
        uint256 _unlockTime,
        bool    _isNFT
    ) external payable whenNotPaused nonReentrant returns (uint256) {
        if (_token == address(0))                              revert InvalidToken();
        if (msg.value < flatFee)                               revert InsufficientFee();
        if (_unlockTime <= block.timestamp)                    revert UnlockInPast();
        if (_unlockTime > block.timestamp + MAX_LOCK_DURATION) revert UnlockTooFar();
        if (_amountOrId == 0)                                  revert InvalidAmount();

        if (_isNFT) {
            IERC721(_token).safeTransferFrom(msg.sender, address(this), _amountOrId);
            activeNFTLocks[_token] += 1;
        } else {
            uint256 before = IERC20(_token).balanceOf(address(this));
            IERC20(_token).safeTransferFrom(msg.sender, address(this), _amountOrId);
            _amountOrId = IERC20(_token).balanceOf(address(this)) - before;
            if (_amountOrId == 0) revert ZeroReceived();
            activeTokenLocks[_token] += 1;
        }

        uint256 lockId = nextLockId++;
        locks[lockId] = Lock({
            owner:      msg.sender,
            token:      _token,
            amountOrId: _amountOrId,
            startTime:  block.timestamp,
            unlockTime: _unlockTime,
            isNFT:      _isNFT,
            active:     true
        });

        _addUserLock(msg.sender, lockId);
        _processFee();

        emit TokenLocked(lockId, msg.sender, _token, _amountOrId);
        return lockId;
    }

    // ── Core: multiSend ───────────────────────────────────────────────────────

    function multiSend(
        address            _token,
        address[] calldata _recipients,
        uint256[] calldata _amounts
    ) external payable whenNotPaused nonReentrant {
        if (_recipients.length == 0 || _recipients.length > MAX_BATCH_SIZE) revert BatchSizeError();
        if (_recipients.length != _amounts.length)                           revert ArrayMismatch();
        if (msg.value < flatFee)                                             revert InsufficientFee();

        uint256 totalSent;
        for (uint256 i; i < _recipients.length; ++i) {
            if (_recipients[i] == address(0)) revert ZeroAddressRecipient();
            IERC20(_token).safeTransferFrom(msg.sender, _recipients[i], _amounts[i]);
            totalSent += _amounts[i];
        }

        _processFee();
        emit MultiSent(_token, _recipients.length, totalSent);
    }

    // ── Core: withdraw ────────────────────────────────────────────────────────

    function withdraw(uint256 _lockId) external nonReentrant {
        Lock storage lock = locks[_lockId];
        if (!lock.active)                      revert LockInactive();
        if (msg.sender != lock.owner)          revert NotLockOwner();
        if (block.timestamp < lock.unlockTime) revert StillLocked();

        address token  = lock.token;
        uint256 amount = lock.amountOrId;
        bool    isNFT  = lock.isNFT;

        delete locks[_lockId];
        _removeUserLock(msg.sender, _lockId);

        if (isNFT) {
            activeNFTLocks[token] -= 1;
            IERC721(token).safeTransferFrom(address(this), msg.sender, amount);
        } else {
            activeTokenLocks[token] -= 1;
            IERC20(token).safeTransfer(msg.sender, amount);
        }

        emit Withdrawn(_lockId, msg.sender, amount);
    }

    // ── Lock transfer ─────────────────────────────────────────────────────────

    function transferLock(uint256 _lockId, address _newOwner) external {
        if (_newOwner == address(0))            revert InvalidAddress();
        if (!locks[_lockId].active)             revert LockInactive();
        if (msg.sender != locks[_lockId].owner) revert NotLockOwner();

        address old = locks[_lockId].owner;
        locks[_lockId].owner = _newOwner;

        _removeUserLock(old, _lockId);
        _addUserLock(_newOwner, _lockId);

        emit LockTransferred(_lockId, old, _newOwner);
    }

    // ── Owner: rescue ─────────────────────────────────────────────────────────

    function rescueStuckTokens(address _token, uint256 _amount) external onlyOwner {
        if (_token == address(0)) {
            uint256 bal = address(this).balance;
            if (bal == 0) revert NothingToRescue();
            payable(owner()).sendValue(bal);
            emit ETHRescued(owner(), bal);
        } else {
            if (activeTokenLocks[_token] > 0) revert ActiveERC20LocksExist();
            if (activeNFTLocks[_token]   > 0) revert ActiveNFTLocksExist();
            if (_supportsERC721(_token))       revert TokenIsNFTContract();
            IERC20(_token).safeTransfer(owner(), _amount);
            emit ERC20Rescued(_token, owner(), _amount);
        }
    }

    function rescueStuckNFT(address _token, uint256 _tokenId) external onlyOwner {
        if (activeNFTLocks[_token] > 0) revert ActiveNFTLocksExist();
        if (!_supportsERC721(_token))   revert TokenIsNotNFTContract();
        IERC721(_token).safeTransferFrom(address(this), owner(), _tokenId);
        emit NFTRescued(_token, owner(), _tokenId);
    }

    // ── View ──────────────────────────────────────────────────────────────────

    function getUserLocks(address _user) external view returns (uint256[] memory) {
        uint256[] storage all = _userLockIds[_user];
        uint256 count;
        for (uint256 i; i < all.length; ++i) {
            if (locks[all[i]].active && locks[all[i]].owner == _user) ++count;
        }
        uint256[] memory result = new uint256[](count);
        uint256 idx;
        for (uint256 i; i < all.length; ++i) {
            if (locks[all[i]].active && locks[all[i]].owner == _user) {
                result[idx++] = all[i];
            }
        }
        return result;
    }

    // ── Admin ─────────────────────────────────────────────────────────────────

    function setFlatFee(uint256 _newFee) external onlyOwner {
        if (_newFee > MAX_FLAT_FEE) revert FeeTooHigh();
        emit FlatFeeUpdated(flatFee, _newFee);
        flatFee = _newFee;
    }

    function setTreasury(address payable _newTreasury) external onlyOwner {
        if (_newTreasury == address(0)) revert InvalidAddress();
        emit TreasuryUpdated(treasury, _newTreasury);
        treasury = _newTreasury;
    }

    function pause()   external onlyOwner { _pause(); }
    function unpause() external onlyOwner { _unpause(); }

    // ── Internal ──────────────────────────────────────────────────────────────

    function _addUserLock(address _user, uint256 _lockId) internal {
        _userLockIndex[_user][_lockId] = _userLockIds[_user].length;
        _userLockIds[_user].push(_lockId);
    }

    function _removeUserLock(address _user, uint256 _lockId) internal {
        uint256[] storage arr = _userLockIds[_user];
        uint256 idx  = _userLockIndex[_user][_lockId];
        uint256 last = arr[arr.length - 1];
        arr[idx]     = last;
        _userLockIndex[_user][last] = idx;
        arr.pop();
        delete _userLockIndex[_user][_lockId];
    }

    function _processFee() internal {
        if (msg.value > 0) {
            (bool ok, ) = treasury.call{value: msg.value}("");
            if (!ok) revert FeeTransferFailed();
        }
    }

    function _supportsERC721(address _token) internal view returns (bool) {
        try IERC165(_token).supportsInterface(type(IERC721).interfaceId)
            returns (bool supported) {
            return supported;
        } catch {
            return false;
        }
    }

    // ── Receive ───────────────────────────────────────────────────────────────

    receive() external payable {}

    // ── ERC-721 receiver ──────────────────────────────────────────────────────

    function onERC721Received(
        address,
        address,
        uint256,
        bytes calldata
    ) external pure override returns (bytes4) {
        return this.onERC721Received.selector;
    }
}
