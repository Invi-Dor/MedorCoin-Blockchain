async function sendFunds(recipientAddress, amount) {
    try {
        // Create the transaction object
        const transaction = {
            from: wallet.address,
            to: recipientAddress,
            value: amount, // Amount in the smallest unit (e.g., wei)
            gasLimit: 21000, // Set a reasonable gas limit
            maxFeePerGas: 1, // Set a fixed gas price; consider dynamic pricing
        };

        // Encode the transaction into a raw hexadecimal format
        const rawHex = encodeTransaction(transaction); // You'll need to define this function

        // Send the raw transaction to your backend API
        const response = await fetch('https://your-api-endpoint/send', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
            },
            body: JSON.stringify([rawHex]),
        });

        const result = await response.json();
        if (response.ok) {
            document.getElementById('sendStatus').textContent = `Transaction sent! TX ID: ${result.result}`;
        } else {
            document.getElementById('sendStatus').textContent = `Error: ${result.error.message}`;
        }
    } catch (error) {
        document.getElementById('sendStatus').textContent = `Failed to send: ${error.message}`;
    }
}

// Attach sendFunds to button click
document.getElementById('btn-send').addEventListener('click', () => {
    const addr = document.getElementById('recipient').value.trim();
    const amount = document.getElementById('amount').value; // Adjust to get actual user input
    if (!addr) {
        sendStatus.textContent = 'Please enter a recipient address.';
        return;
    }
    
    if (!wallet || !wallet.address) {
        sendStatus.textContent = 'No wallet address available. Generate one first.';
        return;
    }

    sendFunds(addr, amount);
});
