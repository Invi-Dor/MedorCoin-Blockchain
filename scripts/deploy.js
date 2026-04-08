async function main() {
  const MedorCoin = await ethers.getContractFactory("MedorCoin");
  const contract = await MedorCoin.deploy();
  
  // Wait for the deployment to finish
  await contract.waitForDeployment();
  
  // Get the global address
  const address = await contract.getAddress();
  console.log("SUCCESS! Contract Address:", address);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
