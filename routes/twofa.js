routes/twofa.cjs
/**
 * File: routes/twofa.cjs
 * MEDORCOIN PRODUCTION AUTHENTICATION & 2FA
 */

const express = require("express");
const nodemailer = require("nodemailer");
const bcrypt = require("bcryptjs");
const jwt = require("jsonwebtoken");
const User = require("../models/User"); // Ensure your Model path is correct

const router = express.Router();

const transporter = nodemailer.createTransport({
  service: "gmail",
  auth: {
    user: process.env.EMAIL_USER,
    pass: process.env.EMAIL_PASS
  }
});

// LOGIN & SEND 2FA (POST /api/auth/login)
router.post("/auth/login", async (req, res) => {
  try {
    const { email, password } = req.body;
    
    // 1. Verify User
    const user = await User.findOne({ email });
    if (!user) return res.status(401).json({ msg: "Invalid credentials" });

    // 2. Verify Password
    const isMatch = await bcrypt.compare(password, user.password);
    if (!isMatch) return res.status(401).json({ msg: "Invalid credentials" });

    // 3. Generate 6-Digit Code
    const twoFACode = Math.floor(100000 + Math.random() * 900000);
    user.twoFACode = twoFACode;
    user.twoFAExpires = Date.now() + 600000; // 10 Min Expiry
    await user.save();

    // 4. Dispatch Email
    await transporter.sendMail({
      from: `"MedorCoin Auth" <${process.env.EMAIL_USER}>`,
      to: email,
      subject: "MedorCoin 2FA Code",
      text: `Your 2FA security code is: ${twoFACode}. This code expires in 10 minutes.`
    });

    res.json({ msg: "2FA_SENT", email: email });
  } catch (err) {
    console.error("[AUTH_ERR]", err);
    res.status(500).json({ msg: "Internal server error" });
  }
});

// CONFIRM 2FA & ISSUE SESSION (POST /api/2fa/confirm)
router.post("/2fa/confirm", async (req, res) => {
  try {
    const { email, code } = req.body;
    const user = await User.findOne({ email });

    if (!user || user.twoFACode != code || Date.now() > user.twoFAExpires) {
      return res.status(400).json({ msg: "Invalid or expired 2FA code" });
    }

    // Clear code and finalize session
    user.twoFACode = null;
    user.twoFAExpires = null;
    await user.save();

    // Sign Industrial JWT
    const token = jwt.sign(
      { id: user._id, wallet: user.walletAddress }, 
      process.env.JWT_SECRET, 
      { expiresIn: "1d" }
    );

    res.json({ 
      token: token, 
      address: user.walletAddress,
      msg: "AUTHENTICATED" 
    });
  } catch (err) {
    console.error("[2FA_CONFIRM_ERR]", err);
    res.status(500).json({ msg: "Internal server error" });
  }
});

module.exports = router;

