const fs = require('fs');
const path = require('path');

function safeReadJSON(p, required = false) {
  try {
    const raw = fs.readFileSync(p, 'utf8');
    return JSON.parse(raw);
  } catch (e) {
    if (required) throw new Error(`Missing or invalid JSON: ${p}\n${e.message}`);
    return {};
  }
}

function loadConfig(root = process.cwd()) {
  const pubPath = path.join(root, 'protocol.json');
  const privPath = path.join(root, 'protocol.private.json');

  const pub = safeReadJSON(pubPath, true);
  const priv = safeReadJSON(privPath, false);

  const env = {
    minerRewardBps: process.env.MINER_REWARD_BPS && Number(process.env.MINER_REWARD_BPS),
    protocolRewardBps: process.env.PROTOCOL_REWARD_BPS && Number(process.env.PROTOCOL_REWARD_BPS),
    baseFeePolicy: process.env.BASE_FEE_POLICY,
    splitBpsToTreasury: process.env.SPLIT_BPS_TREASURY && Number(process.env.SPLIT_BPS_TREASURY),
    splitBpsToMiners: process.env.SPLIT_BPS_MINERS && Number(process.env.SPLIT_BPS_MINERS),
    protocolTreasuryAddress: process.env.PROTOCOL_TREASURY_ADDRESS
  };

  return { ...pub, ...priv, ...Object.fromEntries(Object.entries(env).filter(([, v]) => v !== undefined && v !== null)) };
}
