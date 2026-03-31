/**
 * File: routes/signup.cjs
 * MEDORCOIN PRODUCTION SIGNUP ROUTE
 */

const express = require("express");
const bcrypt = require("bcryptjs");
const jwt = require("jsonwebtoken");
const User = require("../models/User"); // Path to your MongoDB/RocksDB user model

const router = express.Router();

router.post("/auth/signup", async (req, res) => {
  try {
    const { email, username, password } = req.body;

    // 1. Check if user already exists
    let user = await User.findOne({ email });
    if (user) {
      return res.status(400).json({ msg: "Email already registered in the blockchain." });
    }

    // 2. Hash Password securely
    const salt = await bcrypt.genSalt(10);
    const hashedPassword = await bcrypt.hash(password, salt);

    // 3. Generate a fresh wallet address for the new miner
    const generatedWallet = "MDR" + require('crypto').randomBytes(16).toString('hex');

    // 4. Create User
    user = new User({
      email,
      username,
      password: hashedPassword,
      walletAddress: generatedWallet
    });

    await user.save();

    // 5. Generate industrial JWT instantly so they don't have to log in again
    const token = jwt.sign(
      { id: user._id, wallet: user.walletAddress },
      process.env.JWT_SECRET || "MEDOR_ROOT_SECRET_KEY",
      { expiresIn: "1d" }
    );

    // 6. Send the successful handshake back to signup.html
    res.status(200).json({
      token: token,
      address: user.walletAddress,
      msg: "ACCOUNT_CREATED"
    });

  } catch (err) {
    console.error("[SIGNUP_ERR]", err);
    res.status(500).json({ msg: "Internal server fault during account creation." });
  }
});

module.exports = router;
