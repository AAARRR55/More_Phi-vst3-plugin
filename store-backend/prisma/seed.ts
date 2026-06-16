import { PrismaClient } from "@prisma/client";
import dotenv from "dotenv";

dotenv.config();

const prisma = new PrismaClient();

async function main() {
  const slug = "more-phi";
  const downloadBase = process.env.DOWNLOAD_URL_BASE ?? "https://cdn.morephi.example/more-phi/";

  await prisma.product.upsert({
    where: { slug },
    update: {},
    create: {
      slug,
      name: "More-Phi",
      description:
        "More-Phi is a JUCE 8-based VST3/AU audio plugin that hosts other plugins and morphs between parameter snapshots using physics-based interpolation, genetic breeding, and AI integration.",
      priceCents: 7900,
      currency: "usd",
      version: "3.3.0",
      downloadUrl: `${downloadBase}MorePhi.vst3.zip`,
      maxActivations: Number(process.env.LICENSE_MAX_ACTIVATIONS ?? 3),
      isActive: true,
    },
  });

  console.log(`Seeded product: ${slug}`);
}

main()
  .catch((e) => {
    console.error(e);
    process.exit(1);
  })
  .finally(() => {
    void prisma.$disconnect();
  });
