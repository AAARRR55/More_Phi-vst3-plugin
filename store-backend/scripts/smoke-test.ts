import { buildApp } from "../src/server.js";

async function main() {
  const app = await buildApp();
  console.log("app built");
  const res = await app.inject({ method: "GET", url: "/health" });
  console.log(res.statusCode, res.body);
  await app.close();
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
