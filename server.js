const express = require("express");
const nodemailer = require("nodemailer");
const app = express();
app.use(express.json());

let generatedCode = "";

app.post("/send-code", async (req,res)=>{
generatedCode = Math.floor(100000 + Math.random() * 900000).toString();

let transporter = nodemailer.createTransport({
service:"gmail",
auth:{
user:"YOUR_EMAIL@gmail.com",
pass:"APP_PASSWORD"
}
});

await transporter.sendMail({
from:"YOUR_EMAIL@gmail.com",
to:req.body.email,
subject:"Your Verification Code",
text:"Your verification code is " + generatedCode
});

res.sendStatus(200);
});

app.post("/verify-code",(req,res)=>{
if(req.body.code === generatedCode){
res.sendStatus(200);
}else{
res.sendStatus(400);
}
});

app.listen(3000);
