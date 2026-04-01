import * as bip39 from "bip39";
import * as bip32 from "bip32";
import * as bitcoin from "bitcoinjs-lib";

// 1) Generate a real BIP39 mnemonic
export function generateMnemonic() {
  return bip39.generateMnemonic(); // 128-bit strength, 12 words
}

// 2) Derive complete wallet from mnemonic
export function deriveWalletFromMnemonic(mnemonic) {
  if (!bip39.validateMnemonic(mnemonic)) {
    throw new Error("Invalid mnemonic");
  }

  // Convert mnemonic -> seed (64 bytes)
  const seed = bip39.mnemonicToSeedSync(mnemonic);

  // Use Bitcoin mainnet
  const network = bitcoin.networks.bitcoin;

  // HD root from seed
  const root = bip32.fromSeed(seed, network);

  // Standard BIP44 path for first BTC address
  const path = "m/44'/0'/0'/0/0";

  // Derive child key
  const child = root.derivePath(path);

  // Get private key in WIF
  const privateKeyWIF = child.toWIF();

  // Build legacy P2PKH address
  const { address } = bitcoin.payments.p2pkh({
    pubkey: child.publicKey,
    network,
  });

  return {
    mnemonic,
    address,
    privateKeyWIF,
  };
}
