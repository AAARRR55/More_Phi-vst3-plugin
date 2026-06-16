import { prisma } from "../db/client.js";
import { licenseKeyHash } from "../lib/crypto.js";
import { ApiError, ErrorCode } from "../lib/errors.js";

export interface ActivateInput {
  licenseKey: string;
  fingerprintHash: string;
  hostname?: string;
  os?: string;
  appVersion?: string;
}

export interface DeactivateInput {
  licenseKey: string;
  fingerprintHash: string;
}

export const ActivationService = {
  async activate(input: ActivateInput) {
    const hash = licenseKeyHash(input.licenseKey);
    const license = await prisma.license.findUnique({
      where: { keyHash: hash },
      include: { activations: true, product: true },
    });

    if (!license) {
      throw new ApiError(ErrorCode.NOT_FOUND, "License not found");
    }

    if (license.status !== "ACTIVE") {
      if (license.status === "REVOKED") {
        throw new ApiError(ErrorCode.LICENSE_REVOKED, "This license has been revoked");
      }
      throw new ApiError(ErrorCode.LICENSE_EXPIRED, "This license is not active");
    }

    const existing = license.activations.find(
      (a) => a.fingerprintHash === input.fingerprintHash && a.status === "ACTIVE"
    );

    if (existing) {
      const updated = await prisma.activation.update({
        where: { id: existing.id },
        data: { lastSeenAt: new Date() },
      });
      return { activation: updated, license, isNew: false };
    }

    const activeCount = license.activations.filter((a) => a.status === "ACTIVE").length;
    if (activeCount >= license.maxActivations) {
      throw new ApiError(
        ErrorCode.MAX_ACTIVATIONS_REACHED,
        `Activation limit reached (${license.maxActivations} machines)`
      );
    }

    const activation = await prisma.activation.create({
      data: {
        licenseId: license.id,
        fingerprintHash: input.fingerprintHash,
        hostname: input.hostname?.trim() || null,
        os: input.os?.trim() || null,
        appVersion: input.appVersion?.trim() || null,
      },
    });

    return { activation, license, isNew: true };
  },

  async deactivate(input: DeactivateInput) {
    const hash = licenseKeyHash(input.licenseKey);
    const license = await prisma.license.findUnique({
      where: { keyHash: hash },
      include: { activations: true },
    });

    if (!license) {
      throw new ApiError(ErrorCode.NOT_FOUND, "License not found");
    }

    const activation = license.activations.find(
      (a) => a.fingerprintHash === input.fingerprintHash && a.status === "ACTIVE"
    );

    if (!activation) {
      throw new ApiError(ErrorCode.NOT_FOUND, "Active activation not found for this machine");
    }

    return prisma.activation.update({
      where: { id: activation.id },
      data: { status: "DEACTIVATED", deactivatedAt: new Date() },
    });
  },

  async refresh(activationId: string, fingerprintHash: string) {
    const activation = await prisma.activation.findFirst({
      where: { id: activationId, fingerprintHash, status: "ACTIVE" },
      include: { license: { include: { product: true } } },
    });

    if (!activation) {
      throw new ApiError(ErrorCode.NOT_FOUND, "Active activation not found");
    }

    if (activation.license.status !== "ACTIVE") {
      throw new ApiError(ErrorCode.LICENSE_REVOKED, "License is not active");
    }

    return prisma.activation.update({
      where: { id: activation.id },
      data: { lastSeenAt: new Date() },
      include: { license: { include: { product: true } } },
    });
  },
};
