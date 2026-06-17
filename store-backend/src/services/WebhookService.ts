import Stripe from "stripe";
import { Prisma } from "@prisma/client";
import { prisma } from "../db/client.js";
import { OrderService } from "./OrderService.js";
import { LicenseService } from "./LicenseService.js";
import { EmailService } from "./EmailService.js";
import { logger } from "../lib/logger.js";
import { ApiError, ErrorCode } from "../lib/errors.js";

export const WebhookService = {
  async handleEvent(event: Stripe.Event) {
    const existing = await prisma.paymentEvent.findUnique({
      where: { stripeEventId: event.id },
    });

    if (existing) {
      logger.info({ stripeEventId: event.id, type: event.type }, "Duplicate Stripe event ignored");
      return { processed: false, reason: "duplicate" };
    }

    const amountCents = this.extractAmountCents(event);
    const orderId = await this.extractOrderId(event);

    await prisma.paymentEvent.create({
      data: {
        orderId,
        stripeEventId: event.id,
        type: event.type,
        amountCents,
        status: this.eventStatus(event),
        rawPayload: event as unknown as Prisma.InputJsonValue,
      },
    });

    switch (event.type) {
      case "checkout.session.completed": {
        await this.handleCheckoutSessionCompleted(event.data.object);
        break;
      }
      case "checkout.session.async_payment_failed":
      case "payment_intent.payment_failed": {
        await this.handlePaymentFailed(event);
        break;
      }
      case "charge.refunded": {
        await this.handleChargeRefunded(event.data.object);
        break;
      }
      default: {
        logger.debug({ type: event.type }, "Unhandled Stripe event type");
      }
    }

    return { processed: true };
  },

  async handleCheckoutSessionCompleted(session: Stripe.Checkout.Session) {
    if (session.payment_status !== "paid") {
      logger.info({ sessionId: session.id }, "Checkout session not paid; skipping fulfillment");
      return;
    }

    const order = await OrderService.findByCheckoutSessionId(session.id);
    if (!order) {
      throw new ApiError(ErrorCode.NOT_FOUND, `Order not found for session ${session.id}`);
    }

    if (order.status === "PAID") {
      logger.info({ orderId: order.id }, "Order already fulfilled");
      return;
    }

    const paymentIntentId =
      typeof session.payment_intent === "string" ? session.payment_intent : session.payment_intent?.id;

    if (!paymentIntentId) {
      throw new ApiError(ErrorCode.INTERNAL_ERROR, "Checkout session missing payment intent");
    }

    // Upsert customer with Stripe customer id if available.
    const stripeCustomerId =
      typeof session.customer === "string" ? session.customer : session.customer?.id;
    if (stripeCustomerId) {
      await prisma.customer.update({
        where: { id: order.customerId },
        data: { stripeCustomerId },
      });
    }

    const licenseResult = await prisma.$transaction(async (tx) => {
      await tx.order.update({
        where: { id: order.id },
        data: {
          status: "PAID",
          paidAt: new Date(),
          stripePaymentIntentId: paymentIntentId,
        },
      });

      return LicenseService.create(
        {
          customerId: order.customerId,
          orderId: order.id,
          productId: order.productId,
          maxActivations: order.product.maxActivations,
        },
        tx
      );
    });

    if (!licenseResult?.license) {
      throw new ApiError(ErrorCode.INTERNAL_ERROR, "License was not created after fulfillment");
    }

    await EmailService.sendLicenseEmail({
      to: order.customer.email,
      licenseKey: licenseResult.plainKey,
      productName: order.product.name,
      downloadUrl: order.product.downloadUrl,
      setPasswordLink: this.buildSetPasswordLink(order.customer.id),
    });

    logger.info(
      { orderId: order.id, licenseId: licenseResult.license.id },
      "Order fulfilled and license emailed"
    );
  },

  async handlePaymentFailed(event: Stripe.Event) {
    const orderId = await this.extractOrderId(event);
    if (!orderId) {
      logger.warn({ type: event.type }, "Could not associate failed payment with an order");
      return;
    }

    const order = await OrderService.findById(orderId);
    if (!order || order.status !== "PENDING") return;

    await OrderService.markFailed(orderId);
    logger.info({ orderId, eventType: event.type }, "Order marked as failed");
  },

  async handleChargeRefunded(charge: Stripe.Charge) {
    if (!charge.payment_intent) return;
    const paymentIntentId =
      typeof charge.payment_intent === "string" ? charge.payment_intent : charge.payment_intent.id;

    const order = await prisma.order.findFirst({
      where: { stripePaymentIntentId: paymentIntentId },
      include: { licenses: true },
    });

    if (!order) {
      logger.warn({ paymentIntentId }, "No order found for refund");
      return;
    }

    await prisma.$transaction([
      prisma.order.update({
        where: { id: order.id },
        data: { status: "REFUNDED", refundedAt: new Date() },
      }),
      ...order.licenses.map((license) =>
        prisma.license.update({
          where: { id: license.id },
          data: { status: "REVOKED", revokedAt: new Date() },
        })
      ),
      prisma.activation.updateMany({
        where: { licenseId: { in: order.licenses.map((l) => l.id) }, status: "ACTIVE" },
        data: { status: "DEACTIVATED", deactivatedAt: new Date() },
      }),
    ]);

    logger.info({ orderId: order.id }, "Order refunded and licenses revoked");
  },

  async extractOrderId(event: Stripe.Event): Promise<string | null> {
    const object = event.data.object as unknown as Record<string, unknown>;

    if (object?.metadata && typeof object.metadata === "object" && "orderId" in object.metadata) {
      return (object.metadata as Record<string, string>).orderId ?? null;
    }

    if (event.type.startsWith("charge.")) {
      const charge = object as unknown as Stripe.Charge;
      if (!charge.payment_intent) return null;
      const paymentIntentId =
        typeof charge.payment_intent === "string" ? charge.payment_intent : charge.payment_intent.id;
      const order = await prisma.order.findFirst({
        where: { stripePaymentIntentId: paymentIntentId },
        select: { id: true },
      });
      return order?.id ?? null;
    }

    if (event.type.startsWith("payment_intent.")) {
      const pi = object as unknown as Stripe.PaymentIntent;
      if (pi.metadata?.orderId) return pi.metadata.orderId;
      // Try to find order via latest charge.
      const chargeId = typeof pi.latest_charge === "string" ? pi.latest_charge : pi.latest_charge?.id;
      if (!chargeId) return null;
      const completedEvent = await prisma.paymentEvent.findFirst({
        where: {
          type: "checkout.session.completed",
          rawPayload: { path: ["data", "object", "payment_intent"], equals: pi.id },
        },
        select: { orderId: true },
      });
      return completedEvent?.orderId ?? null;
    }

    return null;
  },

  extractAmountCents(event: Stripe.Event): number | null {
    const object = event.data.object as unknown as Record<string, unknown>;
    if (typeof object?.amount === "number") {
      return object.amount;
    }
    if (typeof object?.amount_total === "number") {
      return object.amount_total;
    }
    return null;
  },

  eventStatus(event: Stripe.Event): string {
    const object = event.data.object as unknown as Record<string, unknown>;
    if (typeof object?.status === "string") return object.status;
    if (event.type === "checkout.session.completed") return "succeeded";
    if (event.type === "checkout.session.async_payment_failed") return "failed";
    return "unknown";
  },

  buildSetPasswordLink(customerId: string): string {
    // In production, generate a signed single-use token.
    const base = process.env.FRONTEND_URL ?? "http://localhost:5173";
    return `${base}/set-password?customer=${customerId}`;
  },
};
