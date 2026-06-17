import { FastifyInstance, FastifyReply } from "fastify";
import { z } from "zod";
import { createHash, randomBytes } from "crypto";
import { CustomerService } from "../../services/CustomerService.js";
import { verifyPassword } from "../../lib/crypto.js";
import {
  signAccessToken,
  signRefreshToken,
  verifyRefreshToken,
} from "../../lib/jwt.js";
import { validate } from "../../lib/validate.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";
import { prisma } from "../../db/client.js";
import { env } from "../../config/env.js";
import { rotateRefreshToken } from "../../plugins/auth.js";

const loginSchema = z.object({
  email: z.string().email(),
  password: z.string().min(1),
});

const TOKEN_COOKIE_OPTIONS = {
  httpOnly: true,
  secure: env.NODE_ENV === "production",
  sameSite: "strict" as const,
  path: "/",
};

function setAuthCookies(reply: FastifyReply, accessToken: string, refreshToken: string) {
  void reply.setCookie("mp_auth", accessToken, {
    ...TOKEN_COOKIE_OPTIONS,
    maxAge: env.JWT_ACCESS_TTL * 1000,
  });
  void reply.setCookie("mp_refresh", refreshToken, {
    ...TOKEN_COOKIE_OPTIONS,
    maxAge: env.JWT_REFRESH_TTL * 1000,
  });
}

function clearAuthCookies(reply: FastifyReply) {
  void reply.clearCookie("mp_auth", TOKEN_COOKIE_OPTIONS);
  void reply.clearCookie("mp_refresh", TOKEN_COOKIE_OPTIONS);
}

export async function authRoutes(app: FastifyInstance) {
  app.post(
    "/login",
    {
      config: {
        rateLimit: {
          max: env.LOGIN_RATE_LIMIT_MAX,
          timeWindow: env.LOGIN_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const input = validate(loginSchema, request.body);
      const customer = await CustomerService.findByEmail(input.email);

      if (!customer || !customer.passwordHash) {
        throw new ApiError(ErrorCode.UNAUTHORIZED, "Invalid email or password");
      }

      const valid = await verifyPassword(input.password, customer.passwordHash);
      if (!valid) {
        throw new ApiError(ErrorCode.UNAUTHORIZED, "Invalid email or password");
      }

      const accessToken = signAccessToken({ sub: customer.id, email: customer.email });
      const jti = randomBytes(32).toString("hex");
      const refreshToken = signRefreshToken(customer.id, jti);
      // Store the hash of the JWT that actually goes into the cookie, so the
      // refresh path (which hashes the presented cookie value) can match it.
      const tokenHash = createHash("sha256").update(refreshToken).digest("hex");

      await prisma.refreshToken.create({
        data: {
          customerId: customer.id,
          tokenHash,
          expiresAt: new Date(Date.now() + env.JWT_REFRESH_TTL * 1000),
        },
      });

      setAuthCookies(reply, accessToken, refreshToken);

      return reply.send({
        accessToken,
        customer: {
          id: customer.id,
          email: customer.email,
        },
      });
    }
  );

  app.post(
    "/refresh",
    {
      config: {
        rateLimit: {
          max: env.LOGIN_RATE_LIMIT_MAX,
          timeWindow: env.LOGIN_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const { customerId, email, refreshToken } = await rotateRefreshToken(request, reply);

      const accessToken = signAccessToken({ sub: customerId, email });

      setAuthCookies(reply, accessToken, refreshToken);

      return reply.send({ accessToken });
    }
  );

  app.post("/logout", async (request, reply) => {
    const refreshToken = request.cookies?.mp_refresh;
    if (refreshToken) {
      try {
        verifyRefreshToken(refreshToken);
        const hash = createHash("sha256").update(refreshToken).digest("hex");
        await prisma.refreshToken.updateMany({
          where: { tokenHash: hash },
          data: { revokedAt: new Date() },
        });
      } catch {
        // Ignore invalid token on logout.
      }
    }

    clearAuthCookies(reply);
    return reply.send({ status: "logged_out" });
  });
}
