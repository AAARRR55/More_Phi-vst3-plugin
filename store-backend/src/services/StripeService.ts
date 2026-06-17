import Stripe from "stripe";
import { env } from "../config/env.js";
import { ApiError, ErrorCode } from "../lib/errors.js";
import type { Product, Customer } from "@prisma/client";
import type { OrderWithCustomerAndProduct } from "./types.js";

export const stripe = new Stripe(env.STRIPE_SECRET_KEY, {
  apiVersion: "2025-02-24.acacia",
  typescript: true,
});

export interface CheckoutSessionResult {
  sessionId: string;
  url: string | null;
}

export const StripeService = {
  async createCheckoutSession(
    product: Product,
    customer: Customer,
    orderId: string
  ): Promise<CheckoutSessionResult> {
    try {
      const session = await stripe.checkout.sessions.create({
        mode: "payment",
        client_reference_id: orderId,
        customer_email: customer.stripeCustomerId ? undefined : customer.email,
        customer: customer.stripeCustomerId || undefined,
        line_items: [
          {
            price_data: {
              currency: product.currency,
              product_data: {
                name: product.name,
                description: `Version ${product.version}`,
              },
              unit_amount: product.priceCents,
            },
            quantity: 1,
          },
        ],
        success_url: env.STRIPE_SUCCESS_URL.replace("{ORDER_ID}", orderId),
        cancel_url: env.STRIPE_CANCEL_URL,
        payment_intent_data: {
          metadata: { orderId },
        },
        metadata: {
          orderId,
          productId: product.id,
          productSlug: product.slug,
        },
      });

      if (!session.url) {
        throw new ApiError(ErrorCode.INTERNAL_ERROR, "Stripe did not return a checkout URL");
      }

      return { sessionId: session.id, url: session.url };
    } catch (error) {
      if (error instanceof ApiError) throw error;
      throw new ApiError(
        ErrorCode.INTERNAL_ERROR,
        error instanceof Error ? error.message : "Failed to create Stripe Checkout Session"
      );
    }
  },

  constructWebhookEvent(payload: Buffer | string, signature: string): Stripe.Event {
    try {
      return stripe.webhooks.constructEvent(payload, signature, env.STRIPE_WEBHOOK_SECRET);
    } catch (error) {
      throw new ApiError(
        ErrorCode.WEBHOOD_INVALID_SIGNATURE,
        error instanceof Error ? error.message : "Invalid Stripe signature"
      );
    }
  },
};

export type { OrderWithCustomerAndProduct };
