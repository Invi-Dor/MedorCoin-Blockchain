module.exports = function(db) {
  const express = require("express");
  const router = express.Router();

  // POST /api/crypto/verify-address
  router.post("/verify-address", async (req, res) => {
    const { userId, coin, address } = req.body;

    if (!userId || !coin || !address) {
      return res.status(400).json({ message: "Missing fields" });
    }

    try {
      // Basic blockchain address validation (dummy for now)
      const isValid = address.startsWith("0x") && address.length === 42;

      // Save to LevelDB for reference
      await db.put(`address:${userId}:${coin}`, { address, verified: isValid });

      res.json({
        userId,
        coin,
        address,
        verified: isValid
      });
    } catch (err) {
      res.status(500).json({ message: "Server error", error: err.message });
    }
  });

  return router;
};
