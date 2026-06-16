import { Prisma } from "@prisma/client";
import { prisma } from "../db/client.js";
import { hashPassword } from "../lib/crypto.js";
import { ApiError, ErrorCode } from "../lib/errors.js";

export interface CreateCustomerInput {
  email: string;
  password?: string;
  name?: string;
  company?: string;
  country?: string;
}

export interface UpdateCustomerInput {
  name?: string;
  company?: string;
  country?: string;
}

export const CustomerService = {
  async create(input: CreateCustomerInput) {
    const passwordHash = input.password ? await hashPassword(input.password) : null;

    try {
      return await prisma.customer.create({
        data: {
          email: input.email.toLowerCase().trim(),
          passwordHash,
          name: input.name?.trim() || null,
          company: input.company?.trim() || null,
          country: input.country?.trim().toUpperCase() || null,
        },
        select: {
          id: true,
          email: true,
          name: true,
          company: true,
          country: true,
          createdAt: true,
        },
      });
    } catch (error) {
      if (error instanceof Prisma.PrismaClientKnownRequestError && error.code === "P2002") {
        throw new ApiError(ErrorCode.CONFLICT, "An account with this email already exists");
      }
      throw error;
    }
  },

  async findById(id: string) {
    return prisma.customer.findUnique({
      where: { id },
      select: {
        id: true,
        email: true,
        name: true,
        company: true,
        country: true,
        createdAt: true,
      },
    });
  },

  async findByEmail(email: string) {
    return prisma.customer.findUnique({
      where: { email: email.toLowerCase().trim() },
    });
  },

  async findOrCreateByEmail(email: string) {
    const existing = await this.findByEmail(email);
    if (existing) return existing;

    return prisma.customer.create({
      data: {
        email: email.toLowerCase().trim(),
      },
    });
  },

  async update(id: string, input: UpdateCustomerInput) {
    return prisma.customer.update({
      where: { id },
      data: {
        name: input.name?.trim() ?? undefined,
        company: input.company?.trim() ?? undefined,
        country: input.country?.trim().toUpperCase() ?? undefined,
      },
      select: {
        id: true,
        email: true,
        name: true,
        company: true,
        country: true,
        createdAt: true,
      },
    });
  },

  async setPassword(id: string, password: string) {
    const passwordHash = await hashPassword(password);
    return prisma.customer.update({
      where: { id },
      data: { passwordHash },
    });
  },
};
