import { FastifyInstance } from "fastify";
import { buildApp } from "../src/server.js";
import { prisma } from "../src/db/client.js";
import { CustomerService } from "../src/services/CustomerService.js";
import { LicenseService } from "../src/services/LicenseService.js";
import { OrderService } from "../src/services/OrderService.js";

export async function buildTestApp(): Promise<FastifyInstance> {
  return buildApp();
}

export async function createCustomer(email: string, password = "Password123!") {
  return CustomerService.create({ email, password });
}

export async function login(app: FastifyInstance, email: string, password = "Password123!") {
  const response = await app.inject({
    method: "POST",
    url: "/v1/auth/login",
    payload: { email, password },
  });
  const body = response.json<{ accessToken: string }>();
  return body.accessToken;
}

export async function createOrder(customerId: string, sessionId = `cs_test_${Date.now()}`) {
  const product = await prisma.product.findUniqueOrThrow({ where: { slug: "more-phi" } });
  return OrderService.create({
    customerId,
    productId: product.id,
    stripeCheckoutSessionId: sessionId,
    amountCents: product.priceCents,
    currency: product.currency,
  });
}

export async function createLicense(customerId: string, orderId: string) {
  const order = await prisma.order.findUniqueOrThrow({
    where: { id: orderId },
    include: { product: true },
  });
  return LicenseService.create({
    customerId,
    orderId,
    productId: order.productId,
    maxActivations: order.product.maxActivations,
  });
}
