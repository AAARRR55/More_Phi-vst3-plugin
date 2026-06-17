import { FastifyInstance } from "fastify";
import { z } from "zod";
import { prisma } from "../../db/client.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";

const paramsSchema = z.object({
  slug: z.string().min(1),
});

export async function productRoutes(app: FastifyInstance) {
  app.get("/:slug", async (request, reply) => {
    const { slug } = paramsSchema.parse(request.params);

    const product = await prisma.product.findUnique({
      where: { slug },
    });

    if (!product || !product.isActive) {
      throw new ApiError(ErrorCode.NOT_FOUND, "Product not found");
    }

    return reply.send({
      slug: product.slug,
      name: product.name,
      description: product.description,
      priceCents: product.priceCents,
      currency: product.currency,
      version: product.version,
      maxActivations: product.maxActivations,
      licensingTerms: `One perpetual license per purchase; valid on up to ${product.maxActivations} machines.`,
    });
  });
}
