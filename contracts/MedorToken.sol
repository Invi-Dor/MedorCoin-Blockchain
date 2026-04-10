// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "@openzeppelin/contracts-upgradeable/token/ERC20/ERC20Upgradeable.sol";
import "@openzeppelin/contracts-upgradeable/access/AccessControlUpgradeable.sol";
import "@openzeppelin/contracts-upgradeable/security/PausableUpgradeable.sol";
import "@openzeppelin/contracts-upgradeable/security/ReentrancyGuardUpgradeable.sol";
import "@openzeppelin/contracts-upgradeable/proxy/utils/UUPSUpgradeable.sol";
import "@openzeppelin/contracts/utils/cryptography/ECDSA.sol";

/**
 * @title MedorToken Industrial Bridge
 * @notice Production-grade ERC20 with Multi-Relayer Consensus & Rate Limiting.
 */

contract MedorToken is 
    Initializable, ERC20Upgradeable, AccessControlUpgradeable, 
    PausableUpgradeable, ReentrancyGuardUpgradeable, UUPSUpgradeable 
{
    using ECDSA for bytes32;

    bytes32 public constant RELAYER_ROLE = keccak256("RELAYER_ROLE");
    bytes32 public constant UPGRADER_ROLE = keccak256("UPGRADER_ROLE");

    // Bridge Configuration
    uint256 public constant MINT_FEE_BPS = 30; // 0.3%
    uint256 public threshold;                 
    address public treasury;

    // Rate Limiting
    uint256 public globalDailyLimit;
    uint256 public currentDailyVolume;
    uint256 public lastResetTimestamp;

    // Replay Protection
    mapping(bytes32 => bool) public processedUtxos;

    event BridgeMinted(address indexed to, uint256 amount, bytes32 indexed utxoId);
    event BridgeBurned(address indexed from, uint256 amount, string medorAddress);

    /// @custom:oz-upgrades-unsafe-allow constructor
    constructor() { _disableInitializers(); }

    /**
     * @dev The 20M/30M split happens here.
     * Your wallet: 1ukpJFf4uz3c3gDmM9JASZaGiV4STJEDfzp
     */
    function initialize(
        address admin, 
        uint256 _threshold, 
        uint256 _dailyLimit,
        address _treasury
    ) public initializer {
        __ERC20_init("Medor Token", "MEDOR");
        __AccessControl_init();
        __Pausable_init();
        __ReentrancyGuard_init();
        __UUPSUpgradeable_init();

        // 1. Roles Assignment
        _grantRole(DEFAULT_ADMIN_ROLE, admin);
        _grantRole(UPGRADER_ROLE, admin);
        
        // 2. Bridge Configuration
        threshold = _threshold;
        globalDailyLimit = _dailyLimit;
        treasury = _treasury;
        lastResetTimestamp = block.timestamp;

        // --- 3. PRIVATE ALLOCATION (20 Million) ---
        // Sent directly to your hardcoded address
        address myAddress = 0x75E642510D48df5fff33d507748f6dE2FaB3592A; 
        _mint(myAddress, 20_000_000 * 10**decimals());

        // --- 4. PUBLIC POOL (30 Million) ---
        // Minted to the contract itself to be distributed via Bridge/Mining
        _mint(address(this), 30_000_000 * 10**decimals());
    }

    // =============================================================================
    // PRODUCTION SWAP LOGIC (EVM SIDE)
    // =============================================================================

    function bridgeMint(
        address to, 
        uint256 amount, 
        string calldata txHash, 
        uint32 index, 
        bytes[] calldata signatures
    ) external whenNotPaused nonReentrant {
        require(signatures.length >= threshold, "Insufficient relayer consensus");
        
        bytes32 utxoId = keccak256(abi.encodePacked(txHash, index));
        require(!processedUtxos[utxoId], "UTXO already processed");

        bytes32 messageHash = ECDSA.toEthSignedMessageHash(
            keccak256(abi.encodePacked(to, amount, utxoId))
        );

        _verifySignatures(messageHash, signatures);
        _updateDailyVolume(amount);



        processedUtxos[utxoId] = true;
        uint256 fee = (amount * MINT_FEE_BPS) / 10000;
        uint256 finalAmount = amount - fee;

        _mint(treasury, fee);
        _mint(to, finalAmount);

        emit BridgeMinted(to, finalAmount, utxoId);
    }

    function bridgeBurn(uint256 amount, string calldata medorAddress) external whenNotPaused nonReentrant {
        _burn(msg.sender, amount);
        emit BridgeBurned(msg.sender, amount, medorAddress);
    }

    // =============================================================================
    // INTERNAL HELPERS
    // =============================================================================

        function _verifySignatures(bytes32 hash, bytes[] calldata signatures) internal view {
        address lastSigner = address(0);
        for (uint256 i = 0; i < signatures.length; i++) {
            address signer = ECDSA.recover(hash, signatures[i]);
            require(hasRole(RELAYER_ROLE, signer), "Unauthorized signer");
            require(signer > lastSigner, "Duplicate/Unordered signatures");
            lastSigner = signer;
        }
    }


    function _updateDailyVolume(uint256 amount) internal {
        if (block.timestamp >= lastResetTimestamp + 1 days) {
            currentDailyVolume = 0;
            lastResetTimestamp = block.timestamp;
        }
        require(currentDailyVolume + amount <= globalDailyLimit, "Global daily bridge limit reached");
        currentDailyVolume += amount;
    }

    function _authorizeUpgrade(address) internal override onlyRole(UPGRADER_ROLE) {}
}
