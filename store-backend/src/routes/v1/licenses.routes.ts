import { createHash } from "crypto";
import { FastifyInstance } from "fastify";
import { z } from "zod";
import { prisma } from "../../db/client.js";
import { ActivationService } from "../../services/ActivationService.js";
import { licenseKeyHash, createCertificate, signActivationCertificate, PRODUCT_PUBLIC_ID } from "../../lib/crypto.js";
import { validate } from "../../lib/validate.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";
import { env } from "../../config/env.js";

const activateSchema = z.object({
  licenseKey: z.string().min(12).max(80),
  machineFingerprint: z.string().min(8).max(512),
  hostname: z.string().max(80).optional(),
  os: z.string().max(80).optional(),
  appVersion: z.string().max(40).optional(),
});

const deactivateSchema = z.object({
  licenseKey: z.string().min(12).max(80),
  machineFingerprint: z.string().min(8).max(512),
});

const refreshSchema = z.object({
  activationId: z.string().min(8).max(80),
  machineFingerprint: z.string().min(8).max(512),
});

const verifySchema = z.object({
  key: z.string().min(12).max(80),
});

function fingerprintHash(fingerprint: string): string {
  return createHash("sha256").update(fingerprint).digest("hex");
}

export async function licenseRoutes(app: FastifyInstance) {
  app.post(
    "/activations",
    {
      config: {
        rateLimit: {
          max: env.ACTIVATION_RATE_LIMIT_MAX,
          timeWindow: env.ACTIVATION_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const input = validate(activateSchema, request.body);
      const hash = fingerprintHash(input.machineFingerprint);

      const { activation, license } = await ActivationService.activate({
        licenseKey: input.licenseKey,
        fingerprintHash: hash,
        hostname: input.hostname,
        os: input.os,
        appVersion: input.appVersion,
      });

      const activeCount = await prisma.activation.count({
        where: { licenseId: license.id, status: "ACTIVE" },
      });

      const certificate = createCertificate(
        license.id,
        activation.id,
        PRODUCT_PUBLIC_ID,
        hash
      );
      const signed = signActivationCertificate(certificate);

      return reply.send({
        status: "ACTIVE",
        activationId: activation.id,
        licenseId: license.id,
        activationsUsed: activeCount,
        maxActivations: license.maxActivations,
        certificate: {
          payload: signed.certificatePayload,
          signature: signed.signature,
          keyId: signed.publicKeyId,
        },
      });
    }
  );

  app.post(
    "/activations/refresh",
    {
      config: {
        rateLimit: {
          max: env.ACTIVATION_RATE_LIMIT_MAX * 4,
          timeWindow: env.ACTIVATION_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const input = validate(refreshSchema, request.body);
      const hash = fingerprintHash(input.machineFingerprint);

      const activation = await ActivationService.refresh(input.activationId, hash);
      const certificate = createCertificate(
        activation.licenseId,
        activation.id,
        PRODUCT_PUBLIC_ID,
        hash
      );
      const signed = signActivationCertificate(certificate);

      return reply.send({
        status: "ACTIVE",
        activationId: activation.id,
        licenseId: activation.licenseId,
        certificate: {
          payload: signed.certificatePayload,
          signature: signed.signature,
          keyId: signed.publicKeyId,
        },
      });
    }
  );

  app.post(
    "/activations/deactivate",
    {
      config: {
        rateLimit: {
          max: env.ACTIVATION_RATE_LIMIT_MAX,
          timeWindow: env.ACTIVATION_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const input = validate(deactivateSchema, request.body);
      const hash = fingerprintHash(input.machineFingerprint);

      await ActivationService.deactivate({
        licenseKey: input.licenseKey,
        fingerprintHash: hash,
      });

      return reply.send({ status: "deactivated" });
    }
  );

  app.get(
    "/verify",
    {
      config: {
        rateLimit: {
          max: env.ACTIVATION_RATE_LIMIT_MAX,
          timeWindow: env.ACTIVATION_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const { key } = validate(verifySchema, request.query);
      const hash = licenseKeyHash(key);

      const license = await prisma.license.findUnique({
        where: { keyHash: hash },
      });

      if (!license) {
        throw new ApiError(ErrorCode.NOT_FOUND, "License not found");
      }

      return reply.send({
        valid: license.status === "ACTIVE",
        status: license.status,
        revokedAt: license.revokedAt,
      });
    }
  );
}
