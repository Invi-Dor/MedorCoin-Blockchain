require("dotenv").config();
const express = require("express");
const mongoose = require("mongoose");
const cors = require("cors");
const { exec } = require("child_process");
const fs = require("fs").promises;
const path = require("path");

const authRoutes = require("./routes/auth");

const app = express();

app.use(cors());
app.use(express.json());

// MongoDB connection
mongoose.connect(process.env.MONGO_URI)
  .then(() => console.log("✅ MongoDB Connected"))
  .catch(err => console.log("❌ MongoDB Error:", err));

// ===== C++ BLOCKCHAIN WALLET API =====
app.post('/api/wallet/generate', (req, res) => {
  // Call your C++ MedorCoin binary to generate REAL wallet
  exec('./MedorCoin --generate-wallet', { cwd: './src' }, (error, stdout, stderr) => {
    if (error) {
      console.error('C++ Wallet Error:', stderr);
      return res.status(500).json({ error: 'Wallet generation failed' });
    }
    
    try {
      const wallet = JSON.parse(stdout);
      res.json({
        success: true,
        address: wallet.address,
        privateKey: wallet.privateKey,
        publicKey: wallet.publicKey
      });
    } catch (e) {
      res.status(500).json({ error: 'Invalid wallet response' });
    }
  });
});

app.post('/api/wallet/balance/:address', async (req, res) => {
  const { address } = req.params;
  exec(`./MedorCoin --balance ${address}`, { cwd: './src' }, (error, stdout) => {
    if (error) return res.status(500).json({ balance: 0 });
    res.json({ address, balance: parseFloat(stdout.trim()) });
  });
});

app.post('/api/wallet/send', (req, res) => {
  const { from, to, amount } = req.body;
  exec(`./MedorCoin --send ${from} ${to} ${amount}`, { cwd: './src' }, (error, stdout) => {
    if (error) return res.status(500).json({ error: 'Transaction failed' });
    res.json({ success: true, txid: stdout.trim() });
  });
});

// ===== BLOCKCHAIN STATUS API =====
app.get('/api/stats', async (req, res) => {
  exec('./MedorCoin --stats', { cwd: './src' }, (error, stdout) => {
    if (error) {
      return res.json({
        blockHeight: 0,
        totalWallets: 0,
        hashrate: '0 H/s',
        transactions: 0
      });
    }
    try {
      const stats = JSON.parse(stdout);
      res.json(stats);
    } catch {
      res.json({ blockHeight: 1, totalWallets: 0, hashrate: '1 H/s', transactions: 0 });
    }
  });
});

app.get('/api/blocks', (req, res) => {
  exec('./MedorCoin --blocks', { cwd: './src' }, (error, stdout) => {
    if (error) return res.json([]);
    try {
      res.json(JSON.parse(stdout));
    } catch {
      res.json([]);
    }
  });
});

// Existing routes
app.use("/api/auth", authRoutes);

// Health check
app.get('/api/health', (req, res) => {
  res.json({ status: 'MedorCoin API running', timestamp: new Date().toISOString() });
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`🚀 MedorCoin Server running on port ${PORT}`);
  console.log(`📱 API Endpoints: /api/wallet/generate, /api/stats, /api/blocks`);
});
