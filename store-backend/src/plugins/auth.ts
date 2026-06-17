import { FastifyInstance, FastifyPluginAsync, FastifyRequest, FastifyReply } from "fastify";
import fp from "fastify-plugin";
import { createHash, randomBytes } from "crypto";
import { verifyAccessToken, signRefreshToken, verifyRefreshToken } from "../lib/jwt.js";
import { ApiError, ErrorCode } from "../lib/errors.js";
import { prisma } from "../db/client.js";
import { env } from "../config/env.js";

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

/**
 * Rotate a presented refresh token with reuse-detection (OAuth2 rotating-refresh
 * posture, spec §8).
 *
 * 1. Look up the token by its SHA-256 hash.
 * 2. If the row is missing / expired  -> reject (401).
 * 3. If the row is ALREADY revoked     -> token reuse (probable theft): revoke
 *    the customer's ENTIRE refresh-token family, then reject (401). After this,
 *    even a legitimately-rotated sibling token becomes invalid, forcing a fresh
 *    login.
 * 4. Otherwise (valid, unrevoked)      -> revoke THIS row and issue the new
 *    token. Revoke-old + insert-new run inside a single Prisma `$transaction`
 *    so a partial failure cannot leave the old token live.
 *
 * The NEW token row is created here (not in the route handler) so the
 * transaction covers both writes. Returns the new refresh token (JWT) plus the
 * customer, so the handler can mint an access token and set cookies without
 * touching token storage again.
 */
export async function rotateRefreshToken(
  req: FastifyRequest,
  _reply: FastifyReply
): Promise<{ customerId: string; email: string; refreshToken: string }> {
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

  if (!stored) {
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Invalid refresh token");
  }

  const now = new Date();

  // Reuse-detection: a token that is already revoked should never be presented
  // again. Treat this as token theft and burn the whole family.
  if (stored.revokedAt) {
    await prisma.refreshToken.updateMany({
      where: { customerId: stored.customerId, revokedAt: null },
      data: { revokedAt: now },
    });
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Refresh token reuse detected");
  }

  if (stored.expiresAt < now) {
    throw new ApiError(ErrorCode.UNAUTHORIZED, "Refresh token revoked or expired");
  }

  // Valid, unrevoked token: revoke it and mint the successor in one
  // transaction so the old row can never survive a failed insert.
  const newJti = randomBytes(32).toString("hex");
  const refreshToken = signRefreshToken(stored.customerId, newJti);
  // Hash the JWT that actually becomes the cookie value, so a subsequent
  // refresh (which hashes the presented cookie) can match this row.
  const newHash = createHash("sha256").update(refreshToken).digest("hex");

  await prisma.$transaction([
    prisma.refreshToken.update({
      where: { id: stored.id },
      data: { revokedAt: now },
    }),
    prisma.refreshToken.create({
      data: {
        customerId: stored.customerId,
        tokenHash: newHash,
        expiresAt: new Date(now.getTime() + env.JWT_REFRESH_TTL * 1000),
      },
    }),
  ]);

  return { customerId: stored.customerId, email: stored.customer.email, refreshToken };
}
