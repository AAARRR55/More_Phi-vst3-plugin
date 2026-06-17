import { FastifyInstance, FastifyRequest } from "fastify";
import rateLimit from "@fastify/rate-limit";
import { env } from "../config/env.js";

export async function registerRateLimit(app: FastifyInstance) {
  await app.register(rateLimit, {
    max: env.RATE_LIMIT_MAX,
    timeWindow: env.RATE_LIMIT_WINDOW_MS,
    keyGenerator: (req: FastifyRequest) => {
      return (req.ip ?? "anonymous") + ":" + req.routerPath;
    },
    errorResponseBuilder: (_req, context) => {
      return {
        error: {
          code: "RATE_LIMITED",
          message: `Rate limit exceeded. Try again in ${context.after}`,
          details: { limit: context.max, window: context.after },
        },
      };
    },
  });
}
