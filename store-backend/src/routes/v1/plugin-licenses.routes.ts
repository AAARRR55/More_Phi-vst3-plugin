import { FastifyInstance } from "fastify";
import { z } from "zod";
import { prisma } from "../../db/client.js";
import { ActivationService } from "../../services/ActivationService.js";
import { createCertificate, signActivationCertificate, PRODUCT_PUBLIC_ID } from "../../lib/crypto.js";
import { validate } from "../../lib/validate.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";
import { env } from "../../config/env.js";

const activateSchema = z.object({
  license_key: z.string().min(12).max(80),
  machine_id: z.string().min(8).max(512),
  plugin_version: z.string().max(40).optional(),
  platform: z.string().max(80).optional(),
  daw: z.string().max(80).optional(),
  product_id: z.string().max(80).optional(),
  request_nonce: z.string().max(80).optional(),
});

const refreshSchema = z.object({
  activation_id: z.string().min(8).max(80),
  machine_id: z.string().min(8).max(512),
  product_id: z.string().max(80).optional(),
});

const deactivateSchema = z.object({
  activation_id: z.string().min(8).max(80),
  machine_id: z.string().min(8).max(512),
  product_id: z.string().max(80).optional(),
});

function createCertificateForActivation(
  licenseId: string,
  activationId: string,
  fingerprintHash: string
) {
  const certificate = createCertificate(licenseId, activationId, PRODUCT_PUBLIC_ID, fingerprintHash);
  return signActivationCertificate(certificate);
}

export async function pluginLicenseRoutes(app: FastifyInstance) {
  app.post(
    "/activate",
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

      const { activation, license } = await ActivationService.activate({
        licenseKey: input.license_key,
        fingerprintHash: input.machine_id,
        hostname: input.daw,
        os: input.platform,
        appVersion: input.plugin_version,
      });

      const activeCount = await prisma.activation.count({
        where: { licenseId: license.id, status: "ACTIVE" },
      });

      const signed = createCertificateForActivation(
        license.id,
        activation.id,
        activation.fingerprintHash
      );

      return reply.send({
        status: "ACTIVE",
        activation_id: activation.id,
        license_id: license.id,
        activations_used: activeCount,
        max_activations: license.maxActivations,
        certificate: {
          payload: signed.certificatePayload,
          signature: signed.signature,
          keyId: signed.publicKeyId,
        },
      });
    }
  );

  app.post(
    "/refresh",
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

      const activation = await ActivationService.refresh(input.activation_id, input.machine_id);
      const signed = createCertificateForActivation(
        activation.licenseId,
        activation.id,
        activation.fingerprintHash
      );

      return reply.send({
        status: "ACTIVE",
        activation_id: activation.id,
        license_id: activation.licenseId,
        certificate: {
          payload: signed.certificatePayload,
          signature: signed.signature,
          keyId: signed.publicKeyId,
        },
      });
    }
  );

  app.post(
    "/deactivate",
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

      const activation = await prisma.activation.findFirst({
        where: { id: input.activation_id, fingerprintHash: input.machine_id },
        include: { license: true },
      });

      if (!activation || !activation.license) {
        throw new ApiError(ErrorCode.NOT_FOUND, "Activation not found");
      }

      await ActivationService.deactivate({
        licenseKey: activation.license.key,
        fingerprintHash: input.machine_id,
      });

      return reply.send({ status: "deactivated" });
    }
  );
}
