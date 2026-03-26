{
  "_comment": "MedorCoin Genesis Block — Production Mainnet v1.0.0",
  "_warning": "DO NOT MODIFY after first deployment — any change creates a fork",
  "_docs": "https://medorcoin.io/docs/genesis",

  "version": 1,
  "chainId": 1337,
  "_chainId_note": "Must match node_config.json chain_id exactly — verified on startup",

  "network": "mainnet",
  "networkMagic": "MEDOR",

  "timestamp": 1700000000,
  "_timestamp_note": "Unix UTC — fixed forever, never regenerated",

  "difficulty": 4,
  "gasLimit": 30000000,
  "baseFeePerGas": 1,
  "nonce": "0x0000000000000000",
  "mixHash": "0x0000000000000000000000000000000000000000000000000000000000000000",
  "parentHash": "0x0000000000000000000000000000000000000000000000000000000000000000",

  "coinbase": {
    "_fix1_note": "Must exactly match treasury_address in node_config.json",
    "address": "REPLACE_WITH_TREASURY_ADDRESS",
    "alias": "medor_treasury"
  },

  "maxSupply": 50000000,
  "initialReward": 55,

  "rewardSchedule": [
    {
      "_comment": "Phase 1 — first 60 days",
      "thresholdSeconds": 5184000,
      "reward": 55
    },
    {
      "_comment": "Phase 2 — all time after that",
      "_fix_overflow_note": "Previous value 18446744073709551615 overflows int64 on most systems — replaced with 100-year ceiling",
      "thresholdSeconds": 3153600000,
      "reward": 30
    }
  ],

  "networkFee": {
    "flatFeeThreshold": 1000,
    "flatFeeAmount": 9,
    "percentFeeAboveThreshold": 2,
    "exemptCoinbase": true,
    "feeRecipient": {
      "_fix1_note": "Must exactly match treasury_address in node_config.json",
      "address": "REPLACE_WITH_TREASURY_ADDRESS",
      "alias": "medor_treasury"
    }
  },

  "rewardSplit": {
    "minerPercent": 90,
    "ownerPercent": 10,
    "_split_check": "minerPercent + ownerPercent must always equal 100",
    "ownerAddress": {
      "_fix1_note": "Must exactly match owner_address in node_config.json",
      "address": "REPLACE_WITH_OWNER_ADDRESS",
      "alias": "medor_owner"
    }
  },

  "alloc": {
    "_fix1_note": "All addresses here must match node_config.json exactly — mismatch causes fork",
    "REPLACE_WITH_TREASURY_ADDRESS": {
      "alias": "medor_treasury",
      "balance": "0",
      "balanceUnit": "MDR",
      "comment": "Network fee treasury — receives all network fees",
      "locked": false
    },
    "REPLACE_WITH_OWNER_ADDRESS": {
      "alias": "medor_owner",
      "balance": "0",
      "balanceUnit": "MDR",
      "comment": "Owner reward address — receives 10 percent of every block reward",
      "locked": false
    }
  },

  "fix3_genesisVerification": {
    "_note": "On node startup, compute genesis hash and compare to expected value below — mismatch means accidental fork",
    "expectedGenesisHash": "REPLACE_WITH_COMPUTED_GENESIS_HASH",
    "hashAlgorithm": "sha256",
    "hashFields": [
      "version",
      "chainId",
      "network",
      "timestamp",
      "difficulty",
      "gasLimit",
      "baseFeePerGas",
      "coinbase.address",
      "maxSupply",
      "initialReward",
      "rewardSchedule",
      "networkFee",
      "rewardSplit.minerPercent",
      "rewardSplit.ownerPercent",
      "rewardSplit.ownerAddress.address",
      "alloc",
      "extraData"
    ],
    "verifyOnStartup": true,
    "haltOnMismatch": true
  },

  "security": {
    "immutable": true,
    "checksumValidation": true,
    "rejectModifiedGenesis": true,
    "genesisSignature": {
      "_note": "Optional Ed25519 signature over genesis hash by founding key — verified by all nodes",
      "enabled": false,
      "signingKeyId": "medor_founding_key_v1",
      "signature": "REPLACE_WITH_FOUNDING_SIGNATURE"
    }
  },

  "consensus": {
    "algorithm": "PoW",
    "blockTimeTargetSecs": 10,
    "difficultyAdjustInterval": 2016,
    "minDifficulty": 4,
    "maxDifficulty": 64,
    "coinbaseMaturityBlocks": 100,
    "maxReorgDepth": 100
  },

  "extraData": "MedorCoin Genesis Block -- Built for the future",
  "extraDataEncoding": "utf8",

  "deployment": {
    "deployedBy": "REPLACE_WITH_DEPLOYER_IDENTITY",
    "deploymentDate": "REPLACE_WITH_ISO_DATE",
    "genesisNodeId": "medor-node-001",
    "auditLog": "logs/audit.log"
  }
}
