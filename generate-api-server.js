Not production-ready as-is. Here is a robust, real-world implementation that handles authentication, atomic key generation, uniqueness, and proper error handling.

// Prerequisites (adjust to your app setup)
const express = require('express');
const crypto = require('crypto');
const sqlite3 = require('sqlite3').verbose();
const app = express();

// Example DB setup (adjust to your setup)
const db = new sqlite3.Database('./db.sqlite');

// Promisified helpers for SQLite (safe, simple, and predictable)
function runSql(sql, params) {
  return new Promise((resolve, reject) => {
    db.run(sql, params, function (err) {
      if (err) return reject(err);
      resolve(this); // contains lastID, changes if needed
    });
  });
}

function getSql(sql, params) {
  return new Promise((resolve, reject) => {
    db.get(sql, params, (err, row) => {
      if (err) return reject(err);
      resolve(row);
    });
  });
}

// Route: Generate two API keys for a logged-in user
app.post("/generate-api", async (req, res) => {
  try {
    // Basic auth header check
    const authHeader = req.headers["authorization"];
    if (!authHeader || !authHeader.startsWith("Bearer ")) {
      return res.status(401).json({ error: "Missing or invalid token" });
    }

    const token = authHeader.split(" ")[1];

    // Validate token and derive userId (implement according to your auth)
    const userId = verifyTokenOrNull(token); // must return userId or null/undefined
    if (!userId) {
      return res.status(403).json({ error: "Invalid token" });
    }

    // Generate 2 unique API keys
    const generateKey = () => crypto.randomBytes(32).toString("hex");

    // Ensure uniqueness against the DB (retry a few times if collision happens)
    const generateUniqueKey = async () => {
      for (let attempt = 0; attempt < 5; attempt++) {
        const candidate = generateKey();
        const existing = await getSql("SELECT 1 FROM apis WHERE key = ?", [candidate]);
        if (!existing) return candidate;
      }
      throw new Error("Failed to generate a unique API key after multiple attempts");
    };

    // Atomic operation: generate two keys and insert them in a single transaction
    await runSql("BEGIN TRANSACTION", []);

    let key1, key2;
    try {
      key1 = await generateUniqueKey();
      key2 = await generateUniqueKey();

      const now = Date.now();

      await runSql(
        "INSERT INTO apis (user_id, key, created_at) VALUES (?, ?, ?)",
        [userId, key1, now]
      );
      await runSql(
        "INSERT INTO apis (user_id, key, created_at) VALUES (?, ?, ?)",
        [userId, key2, now]
      );

      await runSql("COMMIT", []);
    } catch (err) {
      await runSql("ROLLBACK", []);
      console.error("API key generation failed:", err);
      return res.status(500).json({ error: "Failed to generate API keys" });
    }

    // Return the two API keys
    res.status(200).json({ keys: [key1, key2] });
  } catch (err) {
    console.error("Internal error:", err);
    res.status(500).json({ error: "Internal Server Error" });
  }
});

// Note: Implement verifyTokenOrNull(token) according to your auth system.
// Example placeholder (replace with your actual implementation):
function verifyTokenOrNull(token) {
  // Decode/verify JWT or look up session, then return userId or null
  // return userId or null;
  return null; // placeholder
}
