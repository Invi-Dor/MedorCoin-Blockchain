#include <cmath>

class Fees {
public:
    // Compute fee for transfers to external wallets (non-platform)
    static double calculateTransferFee(double amount) {
        if (amount >= 10.0 && amount <= 30.0) {
            return round(amount * 0.02 * 100.0) / 100.0; // 2%
        } else if (amount > 30.0 && amount <= 100.0) {
            return round(amount * 0.022 * 100.0) / 100.0; // 2.2%
        } else if (amount > 100.0 && amount <= 300.0) {
            return 4.0; // fixed $4
        } else if (amount < 10.0) {
            return 0.0; // no fee for < $10
        } else {
            return round(amount * 0.015 * 100.0) / 100.0; // >$300 → 1.5%
        }
    }

    // Calculate platform cut from miner rewards (per block mined)
    static double calculatePlatformCut(double minerReward) {
        return round(minerReward * 0.02 * 100.0) / 100.0; // 2% platform cut
    }

    // Calculate net amount received by recipient
    // `isInternal` true if transfer within platform (no fee)
    static double netReceived(double amount, bool isInternal = false) {
        if (isInternal) {
            return amount; // no fee for internal transfers
        }
        double fee = calculateTransferFee(amount);
        return amount - fee;
    }
};
