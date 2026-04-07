async function main() {
  const MedorCoin = await ethers.getContractFactory("MedorCoin");
  const contract = await MedorCoin.deploy();
  await contract.deployed();
  console.log("SUCCESS! Contract Address:", contract.address);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
