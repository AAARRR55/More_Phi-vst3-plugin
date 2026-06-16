import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import Stripe from "stripe";
import { buildTestApp } from "./helpers.js";
import { stripe } from "../src/services/StripeService.js";

describe("Checkout routes", () => {
  beforeEach(() => {
    vi.spyOn(stripe.checkout.sessions, "create").mockResolvedValue({
      id: "cs_test_session",
      url: "https://checkout.stripe.com/test",
    } as unknown as Stripe.Response<Stripe.Checkout.Session>);
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it("creates a checkout session for a guest", async () => {
    const app = await buildTestApp();
    const response = await app.inject({
      method: "POST",
      url: "/v1/checkout",
      payload: { productSlug: "more-phi", email: "checkout-test@example.com" },
    });

    expect(response.statusCode).toBe(200);
    const body = response.json<{ checkoutUrl: string; orderId: string }>();
    expect(body.checkoutUrl).toBe("https://checkout.stripe.com/test");
    expect(body.orderId).toBeDefined();
  });

  it("returns 404 for unknown product", async () => {
    const app = await buildTestApp();
    const response = await app.inject({
      method: "POST",
      url: "/v1/checkout",
      payload: { productSlug: "unknown-product", email: "checkout-test@example.com" },
    });

    expect(response.statusCode).toBe(404);
  });
});
