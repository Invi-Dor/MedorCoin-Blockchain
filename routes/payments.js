module.exports = function (db, stripe) {
  const express = require("express");
  const router = express.Router();

  router.post("/fiat/intent", async (req, res) => {
    try {
      const { amount } = req.body;
      const intent = await stripe.paymentIntents.create({
        amount,
        currency: "usd",
        payment_method_types: ["card"]
      });

      await db.put(`payment:${intent.id}`, { status: "created" });

      res.json({
        paymentId: intent.id,
        clientSecret: intent.client_secret
      });
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  });

  router.post("/fiat/confirm", async (req, res) => {
    try {
      const { paymentId } = req.body;
      const pay = await stripe.paymentIntents.retrieve(paymentId);

      await db.put(`payment:${paymentId}`, {
        status: pay.status
      });

      res.json({ paymentId, status: pay.status });
    } catch (err) {
  console.error(err); // logs locally in terminal / Codespace
  res.status(500).json({ msg: "Internal server error" }); // generic to client
}
  });

  return router;
};
