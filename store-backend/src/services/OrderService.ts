import { OrderStatus, Prisma } from "@prisma/client";
import { prisma } from "../db/client.js";
import { ApiError, ErrorCode } from "../lib/errors.js";

export interface CreateOrderInput {
  customerId: string;
  productId: string;
  stripeCheckoutSessionId: string;
  amountCents: number;
  currency: string;
}

export const OrderService = {
  async create(input: CreateOrderInput) {
    return prisma.order.create({
      data: {
        customerId: input.customerId,
        productId: input.productId,
        stripeCheckoutSessionId: input.stripeCheckoutSessionId,
        amountCents: input.amountCents,
        currency: input.currency.toLowerCase(),
        status: OrderStatus.PENDING,
      },
      include: {
        product: true,
        customer: {
          select: { id: true, email: true },
        },
      },
    });
  },

  async findById(id: string) {
    return prisma.order.findUnique({
      where: { id },
      include: {
        product: true,
        licenses: true,
        paymentEvents: true,
      },
    });
  },

  async findByCheckoutSessionId(sessionId: string) {
    return prisma.order.findUnique({
      where: { stripeCheckoutSessionId: sessionId },
      include: {
        product: true,
        customer: true,
        licenses: true,
      },
    });
  },

  async markPaid(id: string, stripePaymentIntentId: string) {
    try {
      return await prisma.order.update({
        where: { id },
        data: {
          status: OrderStatus.PAID,
          paidAt: new Date(),
          stripePaymentIntentId,
        },
        include: {
          product: true,
          customer: true,
          licenses: true,
        },
      });
    } catch (error) {
      if (error instanceof Prisma.PrismaClientKnownRequestError && error.code === "P2025") {
        throw new ApiError(ErrorCode.NOT_FOUND, "Order not found");
      }
      throw error;
    }
  },

  async markFailed(id: string) {
    return prisma.order.update({
      where: { id },
      data: { status: OrderStatus.FAILED },
    });
  },

  async markRefunded(id: string) {
    return prisma.order.update({
      where: { id },
      data: {
        status: OrderStatus.REFUNDED,
        refundedAt: new Date(),
      },
    });
  },
};
