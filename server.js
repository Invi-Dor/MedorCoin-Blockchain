const express = require("express");
const bodyParser = require("body-parser");
const level = require("level");
const jwt = require("jsonwebtoken");

require("dotenv").config();

const app = express();
app.use(bodyParser.json());

// LevelDB setup
const db = level("./medorcoin-db", { valueEncoding: "json" });

// Stripe
const stripe = require("stripe")(process.env.STRIPE_SECRET_KEY);

// Email
const nodemailer = require("nodemailer");
const transporter = nodemailer.createTransport({
  service: "gmail",
  auth: {
    user: process.env.EMAIL_USER,
    pass: process.env.EMAIL_PASS
  }
});

// Import routes
const authRoutes = require("./routes/auth")(db, jwt, transporter);
const paymentRoutes = require("./routes/payments")(db, stripe);
const miningRoutes = require("./routes/mining")(db);

app.use("/api/auth", authRoutes);
app.use("/api/payments", paymentRoutes);
app.use("/api/mining", miningRoutes);

const PORT = process.env.PORT || 5000;
app.listen(PORT, () => console.log(`Server running on ${PORT}`));
