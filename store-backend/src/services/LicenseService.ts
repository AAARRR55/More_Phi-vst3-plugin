import { LicenseStatus, Prisma } from "@prisma/client";
import { prisma } from "../db/client.js";
import { generateLicenseKey, licenseKeyHash } from "../lib/crypto.js";
import { ApiError, ErrorCode } from "../lib/errors.js";

export interface CreateLicenseInput {
  customerId: string;
  orderId: string;
  productId: string;
  maxActivations?: number;
}

export const LicenseService = {
  async create(input: CreateLicenseInput, tx?: Prisma.TransactionClient, maxAttempts = 5) {
    let lastError: unknown;
    const db = tx ?? prisma;

    for (let attempt = 0; attempt < maxAttempts; attempt++) {
      const key = generateLicenseKey();
      const hash = licenseKeyHash(key);

      try {
        const license = await db.license.create({
          data: {
            customerId: input.customerId,
            orderId: input.orderId,
            productId: input.productId,
            key,
            keyHash: hash,
            maxActivations: input.maxActivations ?? 3,
            status: LicenseStatus.ACTIVE,
          },
          include: {
            product: {
              select: { slug: true, name: true, version: true },
            },
          },
        });
        return { license, plainKey: key };
      } catch (error) {
        lastError = error;
        // Collision is astronomically unlikely; retry.
        continue;
      }
    }

    throw new ApiError(
      ErrorCode.INTERNAL_ERROR,
      `Failed to generate unique license key after ${maxAttempts} attempts`,
      { cause: lastError }
    );
  },

  async findByKeyHash(keyHash: string) {
    return prisma.license.findUnique({
      where: { keyHash },
      include: {
        activations: true,
        product: true,
      },
    });
  },

  async findByCustomerId(customerId: string) {
    return prisma.license.findMany({
      where: { customerId },
      include: {
        activations: {
          where: { status: "ACTIVE" },
          select: { id: true, fingerprintHash: true, activatedAt: true },
        },
        product: {
          select: { slug: true, name: true, version: true, downloadUrl: true },
        },
      },
      orderBy: { createdAt: "desc" },
    });
  },

  async revoke(licenseId: string) {
    return prisma.$transaction([
      prisma.license.update({
        where: { id: licenseId },
        data: { status: LicenseStatus.REVOKED, revokedAt: new Date() },
      }),
      prisma.activation.updateMany({
        where: { licenseId, status: "ACTIVE" },
        data: { status: "DEACTIVATED", deactivatedAt: new Date() },
      }),
    ]);
  },
};
