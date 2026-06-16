import { FastifyInstance, FastifyPluginAsync, FastifyRequest, FastifyReply } from "fastify";
import fp from "fastify-plugin";
import { createHash } from "crypto";
import { verifyAccessToken, verifyRefreshToken } from "../lib/jwt.js";
import { ApiError, ErrorCode } from "../lib/errors.js";
import { prisma } from "../db/client.js";

declare module "fastify" {
  interface FastifyRequest {
    customer?: {
      id: string;
      email: string;
    };
  }
}

async function authenticateRequest(req: FastifyRequest): Promise<{ id: string; email: string } | null> {
  const header = req.headers.authorization;
  let token: string | undefined;

  if (header?.startsWith("Bearer ")) {
    token = header.slice(7);
  } else if (req.cookies?.mp_auth) {
    token = req.cookies.mp_auth;
  }

  if (!token) return null;

  try {
    const payload = verifyAccessToken(token);
    return { id: payload.sub, email: payload.email };
  } catch {
    return null;
  }
}

export async function requireAuth(req: FastifyRequest): Promise<{ id: string; email: string }> {
  const customer = await authenticateRequest(req);
  if (!customer) {
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Authentication required");
  }
  return customer;
}

const authPlugin: FastifyPluginAsync = async (app: FastifyInstance) => {
  app.decorateRequest("customer", undefined);

  app.addHook("onRequest", async (req) => {
    const customer = await authenticateRequest(req);
    if (customer) {
      req.customer = customer;
    }
  });
};

export const registerAuth = fp(authPlugin);

export async function rotateRefreshToken(
  req: FastifyRequest,
  _reply: FastifyReply
): Promise<{ customerId: string; email: string }> {
  const token = req.cookies?.mp_refresh;
  if (!token) {
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Refresh token required");
  }

  try {
    verifyRefreshToken(token);
  } catch {
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Invalid refresh token");
  }

  const hash = createHash("sha256").update(token).digest("hex");
  const stored = await prisma.refreshToken.findUnique({
    where: { tokenHash: hash },
    include: { customer: true },
  });

  if (!stored || stored.revokedAt || stored.expiresAt < new Date()) {
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Refresh token revoked or expired");
  }

  return { customerId: stored.customerId, email: stored.customer.email };
}
