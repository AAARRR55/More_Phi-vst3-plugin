import { FastifyInstance } from "fastify";
import { z } from "zod";
import { prisma } from "../../db/client.js";
import { CustomerService } from "../../services/CustomerService.js";
import { OrderService } from "../../services/OrderService.js";
import { StripeService } from "../../services/StripeService.js";
import { validate } from "../../lib/validate.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";
import { env } from "../../config/env.js";

const checkoutSchema = z.object({
  productSlug: z.string().min(1),
  email: z.string().email(),
});

export async function checkoutRoutes(app: FastifyInstance) {
  app.post(
    "/",
    {
      config: {
        rateLimit: {
          max: env.CHECKOUT_RATE_LIMIT_MAX,
          timeWindow: env.CHECKOUT_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const input = validate(checkoutSchema, request.body);

      const product = await prisma.product.findUnique({
        where: { slug: input.productSlug },
      });

      if (!product || !product.isActive) {
        throw new ApiError(ErrorCode.NOT_FOUND, "Product not found");
      }

      const customer = await CustomerService.findOrCreateByEmail(input.email);

      const order = await OrderService.create({
        customerId: customer.id,
        productId: product.id,
        stripeCheckoutSessionId: "pending", // placeholder, updated after Stripe call
        amountCents: product.priceCents,
        currency: product.currency,
      });

      const { sessionId, url } = await StripeService.createCheckoutSession(product, customer, order.id);

      await prisma.order.update({
        where: { id: order.id },
        data: { stripeCheckoutSessionId: sessionId },
      });

      return reply.send({ checkoutUrl: url, orderId: order.id });
    }
  );
}
