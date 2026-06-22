import {
  createCertificate,
  signActivationCertificate,
  verifyPassword,
  hashPassword,
  generateLicenseKey,
  licenseKeyHash,
} from "../src/lib/crypto.js";

async function main() {
  const cert = createCertificate("lic1", "act1", "prod1", "machine1");
  const signed = signActivationCertificate(cert);
  console.log("Signed certificate:", signed);

  const pwHash = await hashPassword("pw");
  const ok = await verifyPassword("pw", pwHash);
  console.log("Password verify:", ok);

  const key = generateLicenseKey();
  console.log("License key:", key, "Hash:", licenseKeyHash(key));
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
