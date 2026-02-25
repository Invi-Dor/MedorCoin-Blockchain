module.exports = function (db) {
  const express = require("express");
  const router = express.Router();

  router.get("/stats", async (req, res) => {
    try {
      const stats = await db.get("mining:stats");
      res.json(stats);
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  });

  return router;
};
