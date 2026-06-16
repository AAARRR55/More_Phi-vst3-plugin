import { beforeAll, beforeEach } from "vitest";
import { prisma } from "../src/db/client.js";
import { env } from "../src/config/env.js";

async function seedProduct() {
  await prisma.product.upsert({
    where: { slug: "more-phi" },
    update: {},
    create: {
      slug: "more-phi",
      name: "More-Phi",
      description: "Test product",
      priceCents: 7900,
      currency: "usd",
      version: "3.3.0",
      downloadUrl: `${env.DOWNLOAD_URL_BASE}MorePhi.vst3.zip`,
      maxActivations: 3,
      isActive: true,
    },
  });
}

async function truncateTables() {
  const tablenames = await prisma.$queryRaw<
    { tablename: string }[]
  >`SELECT tablename FROM pg_tables WHERE schemaname='public'`;

  const tables = tablenames
    .map(({ tablename }) => tablename)
    .filter((name) => !name.startsWith("_"));

  for (const table of tables) {
    await prisma.$executeRawUnsafe(`TRUNCATE TABLE "${table}" CASCADE;`);
  }
}

beforeAll(async () => {
  await truncateTables();
  await seedProduct();
});

beforeEach(async () => {
  // Clean all tables except products between tests.
  const tables = [
    "activations",
    "licenses",
    "payment_events",
    "orders",
    "refresh_tokens",
    "customers",
  ];
  for (const table of tables) {
    await prisma.$executeRawUnsafe(`TRUNCATE TABLE "${table}" CASCADE;`);
  }
});
