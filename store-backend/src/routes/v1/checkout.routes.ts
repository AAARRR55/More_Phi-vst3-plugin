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
import { LicenseService } from "../../services/LicenseService.js";

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

      let activeOrder = order;

      if (env.NODE_ENV === "development" && activeOrder.status === "PENDING") {
        const paymentIntentId = "pi_mock_" + Math.random().toString(36).substring(7);
        await prisma.$transaction(async (tx) => {
          const updatedOrder = await tx.order.update({
            where: { id: activeOrder.id },
            data: {
              status: "PAID",
              paidAt: new Date(),
              stripePaymentIntentId: paymentIntentId,
            },
            include: {
              product: true,
            },
          });

          await LicenseService.create(
            {
              customerId: activeOrder.customerId,
              orderId: activeOrder.id,
              productId: activeOrder.productId,
              maxActivations: updatedOrder.product.maxActivations,
            },
            tx
          );
        });

        const refreshed = await prisma.order.findUnique({
          where: { id: activeOrder.id },
          include: {
            licenses: {
              include: {
                activations: { where: { status: "ACTIVE" } },
              },
            },
          },
        });
        if (refreshed) {
          activeOrder = refreshed;
        }
      }

      const license = activeOrder.licenses[0];

      return reply.send({
        status: activeOrder.status,
        payment_status: mapPaymentStatus(activeOrder.status),
        order: {
          id: activeOrder.id,
          product_id: activeOrder.productId,
          amount: activeOrder.amountCents / 100,
          currency: activeOrder.currency,
          status: activeOrder.status,
          created_at: activeOrder.createdAt.toISOString(),
          checkout_session_id: activeOrder.stripeCheckoutSessionId,
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
        amount_total: activeOrder.amountCents,
        currency: activeOrder.currency,
      });
    }
  );
}
