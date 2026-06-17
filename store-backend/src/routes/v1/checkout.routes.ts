import { FastifyInstance } from "fastify";
import { z } from "zod";
import { prisma } from "../../db/client.js";
import { CustomerService } from "../../services/CustomerService.js";
import { OrderService } from "../../services/OrderService.js";
import { StripeService } from "../../services/StripeService.js";
import { requireAuth } from "../../plugins/auth.js";
import { validate } from "../../lib/validate.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";
import { env } from "../../config/env.js";

const checkoutSchema = z.object({
  productSlug: z.string().min(1),
  email: z.string().email(),
});

const paramsSchema = z.object({
  sessionId: z.string().min(1),
});

function mapPaymentStatus(status: string): string {
  switch (status) {
    case "PAID":
      return "paid";
    case "FAILED":
      return "unpaid";
    case "REFUNDED":
      return "unpaid";
    default:
      return "unpaid";
  }
}

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

      await prisma.order.deleteMany({
        where: {
          customerId: customer.id,
          productId: product.id,
          status: "PENDING",
          stripeCheckoutSessionId: { startsWith: "pending-" },
        },
      });

      const order = await OrderService.create({
        customerId: customer.id,
        productId: product.id,
        stripeCheckoutSessionId: `pending-${Date.now()}`, // placeholder, updated after Stripe call
        amountCents: product.priceCents,
        currency: product.currency,
      });

      const { sessionId, url } = await StripeService.createCheckoutSession(product, customer, order.id);

      await prisma.order.update({
        where: { id: order.id },
        data: { stripeCheckoutSessionId: sessionId },
      });

      return reply.send({ checkoutUrl: url, orderId: order.id, sessionId });
    }
  );

  app.get(
    "/status/:sessionId",
    {
      config: {
        rateLimit: {
          max: env.CHECKOUT_RATE_LIMIT_MAX,
          timeWindow: env.CHECKOUT_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const customer = await requireAuth(request);
      const { sessionId } = validate(paramsSchema, request.params);

      const order = await prisma.order.findUnique({
        where: { stripeCheckoutSessionId: sessionId },
        include: {
          licenses: {
            include: {
              activations: { where: { status: "ACTIVE" } },
            },
          },
        },
      });

      if (!order || order.customerId !== customer.id) {
        throw new ApiError(ErrorCode.NOT_FOUND, "Order not found");
      }

      const license = order.licenses[0];

      return reply.send({
        status: order.status,
        payment_status: mapPaymentStatus(order.status),
        order: {
          id: order.id,
          product_id: order.productId,
          amount: order.amountCents / 100,
          currency: order.currency,
          status: order.status,
          created_at: order.createdAt.toISOString(),
          checkout_session_id: order.stripeCheckoutSessionId,
        },
        license: license
          ? {
              id: license.id,
              license_key: license.key,
              product_id: license.productId,
              status: license.status,
              activation_limit: license.maxActivations,
              created_at: license.createdAt.toISOString(),
              activations: license.activations.map((a) => ({
                id: a.id,
                machine_id: a.fingerprintHash,
                daw: a.os || undefined,
                activated_at: a.activatedAt.toISOString(),
                status: a.status,
              })),
            }
          : null,
        amount_total: order.amountCents,
        currency: order.currency,
      });
    }
  );
}
