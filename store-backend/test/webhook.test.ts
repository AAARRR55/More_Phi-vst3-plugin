import { describe, it, expect, beforeEach, vi } from "vitest";
import Stripe from "stripe";
import { buildTestApp, createCustomer, createOrder, createLicense } from "./helpers.js";
import { stripe } from "../src/services/StripeService.js";
import { env } from "../src/config/env.js";
import { prisma } from "../src/db/client.js";

describe("Stripe webhook", () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  function buildSessionEvent(
    sessionId: string,
    paymentIntentId: string,
    customerEmail: string
  ): Stripe.Checkout.Session {
    return {
      id: sessionId,
      object: "checkout.session",
      payment_status: "paid",
      payment_intent: paymentIntentId,
      customer_email: customerEmail,
      metadata: { orderId: "placeholder" },
    } as unknown as Stripe.Checkout.Session;
  }

  function signEvent(event: Stripe.Event): { payload: string; signature: string } {
    const payload = JSON.stringify(event);
    const signature = stripe.webhooks.generateTestHeaderString({
      payload,
      secret: env.STRIPE_WEBHOOK_SECRET,
    });
    return { payload, signature };
  }

  it("fulfills an order on checkout.session.completed", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("webhook-test@example.com");
    const order = await createOrder(customer.id, "cs_webhook_session");

    const event = {
      id: "evt_webhook_1",
      object: "event",
      type: "checkout.session.completed",
      data: {
        object: buildSessionEvent("cs_webhook_session", "pi_webhook_1", customer.email),
      },
    } as unknown as Stripe.Event;
    (event.data.object as unknown as { metadata: Record<string, string> }).metadata.orderId = order.id;

    const { payload, signature } = signEvent(event);

    const response = await app.inject({
      method: "POST",
      url: "/v1/webhooks/stripe",
      headers: { "stripe-signature": signature, "content-type": "application/json" },
      payload,
    });

    expect(response.statusCode).toBe(200);
    const updatedOrder = await prisma.order.findUnique({
      where: { id: order.id },
      include: { licenses: true },
    });
    expect(updatedOrder?.status).toBe("PAID");
    expect(updatedOrder?.licenses.length).toBe(1);
  });

  it("ignores duplicate events", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("webhook-dup@example.com");
    const order = await createOrder(customer.id, "cs_webhook_dup");

    const event = {
      id: "evt_webhook_dup",
      object: "event",
      type: "checkout.session.completed",
      data: {
        object: buildSessionEvent("cs_webhook_dup", "pi_webhook_dup", customer.email),
      },
    } as unknown as Stripe.Event;
    (event.data.object as unknown as { metadata: Record<string, string> }).metadata.orderId = order.id;

    const { payload, signature } = signEvent(event);

    await app.inject({
      method: "POST",
      url: "/v1/webhooks/stripe",
      headers: { "stripe-signature": signature, "content-type": "application/json" },
      payload,
    });

    const second = await app.inject({
      method: "POST",
      url: "/v1/webhooks/stripe",
      headers: { "stripe-signature": signature, "content-type": "application/json" },
      payload,
    });

    expect(second.statusCode).toBe(200);
    const count = await prisma.paymentEvent.count({ where: { stripeEventId: "evt_webhook_dup" } });
    expect(count).toBe(1);
  });

  it("revokes license on charge.refunded", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("webhook-refund@example.com");
    const order = await createOrder(customer.id, "cs_webhook_refund");
    await prisma.order.update({
      where: { id: order.id },
      data: { status: "PAID", stripePaymentIntentId: "pi_refund" },
    });
    const { license } = await createLicense(customer.id, order.id);
    await prisma.activation.create({
      data: {
        licenseId: license.id,
        fingerprintHash: "hash1",
      },
    });

    const event = {
      id: "evt_refund",
      object: "event",
      type: "charge.refunded",
      data: {
        object: { id: "ch_refund", object: "charge", payment_intent: "pi_refund" },
      },
    } as unknown as Stripe.Event;

    const { payload, signature } = signEvent(event);

    const response = await app.inject({
      method: "POST",
      url: "/v1/webhooks/stripe",
      headers: { "stripe-signature": signature, "content-type": "application/json" },
      payload,
    });

    expect(response.statusCode).toBe(200);
    const updatedLicense = await prisma.license.findUnique({ where: { id: license.id } });
    expect(updatedLicense?.status).toBe("REVOKED");
    const activeActivations = await prisma.activation.count({
      where: { licenseId: license.id, status: "ACTIVE" },
    });
    expect(activeActivations).toBe(0);
  });
});
