module.exports = function (db, jwt, transporter) {
  const express = require("express");
  const bcrypt = require("bcryptjs");
  const router = express.Router();

  router.post("/register", async (req, res) => {
    try {
      const { username, email, password } = req.body;
      const hashed = await bcrypt.hash(password, 10);

      await db.put(`user:${email}`, {
        username,
        email,
        password: hashed,
        verified: false
      });

      const code = Math.floor(100000 + Math.random() * 900000);
      await db.put(`verify:${email}`, code);

      await transporter.sendMail({
        from: process.env.EMAIL_USER,
        to: email,
        subject: "Verify Your Email",
        text: `Your code is ${code}`
      });

      res.json({ msg: "Verification sent" });
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  });

  router.post("/verify", async (req, res) => {
    try {
      const { email, code } = req.body;
      const stored = await db.get(`verify:${email}`);
      if (stored != code) return res.status(400).json({ msg: "Invalid code" });

      const user = await db.get(`user:${email}`);
      user.verified = true;
      await db.put(`user:${email}`, user);

      const token = jwt.sign({ email }, process.env.JWT_SECRET);
      res.json({ token });
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  });

  router.post("/login", async (req, res) => {
    try {
      const { email, password } = req.body;
      const user = await db.get(`user:${email}`);
      const match = await bcrypt.compare(password, user.password);
      if (!match) return res.status(400).json({ msg: "Invalid" });

      const token = jwt.sign({ email }, process.env.JWT_SECRET);
      res.json({ token });
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  });

  return router;
};
