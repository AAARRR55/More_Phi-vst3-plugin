import Fastify, { FastifyInstance, FastifyBaseLogger, FastifyRequest } from "fastify";
import { randomUUID } from "crypto";
import { fileURLToPath } from "url";
import cookie from "@fastify/cookie";
import { env } from "./config/env.js";
import { logger } from "./lib/logger.js";
import { registerCors } from "./plugins/cors.js";
import { registerHelmet } from "./plugins/helmet.js";
import { registerRateLimit } from "./plugins/rateLimit.js";
import { registerAuth } from "./plugins/auth.js";
import { registerErrorHandler } from "./plugins/errorHandler.js";
import { registerRequestId } from "./plugins/requestId.js";
import { productRoutes } from "./routes/v1/products.routes.js";
import { customerRoutes } from "./routes/v1/customers.routes.js";
import { authRoutes } from "./routes/v1/auth.routes.js";
import { checkoutRoutes } from "./routes/v1/checkout.routes.js";
import { stripeWebhookRoutes } from "./routes/v1/webhooks.stripe.routes.js";
import { licenseRoutes } from "./routes/v1/licenses.routes.js";
import { pluginLicenseRoutes } from "./routes/v1/plugin-licenses.routes.js";
import { meRoutes } from "./routes/v1/me.routes.js";
import { healthRoutes } from "./routes/v1/health.routes.js";

export async function buildApp(): Promise<FastifyInstance> {
  const app = Fastify({
    logger: logger as FastifyBaseLogger,
    genReqId: () => randomUUID(),
    requestTimeout: 30000,
  });

  // Capture raw body for Stripe signature verification before JSON parsing.
  app.addContentTypeParser(
    "application/json",
    { parseAs: "buffer" },
    (req: FastifyRequest, body: Buffer, done: (err: Error | null, body?: unknown) => void) => {
      req.rawBody = body;
      if (body.length === 0) {
        done(null, undefined);
        return;
      }
      try {
        const parsed: unknown = JSON.parse(body.toString("utf8"));
        done(null, parsed);
      } catch (error) {
        done(error as Error, undefined);
      }
    }
  );

  registerErrorHandler(app);
  registerRequestId(app);

  await app.register(cookie, {
    secret: env.JWT_SECRET,
    parseOptions: {
      httpOnly: true,
      secure: env.NODE_ENV === "production",
      sameSite: "strict",
      path: "/",
    },
  });

  await registerCors(app);
  await registerHelmet(app);
  await registerRateLimit(app);
  await app.register(registerAuth);

  await app.register(healthRoutes, { prefix: "/health" });
  await app.register(pluginLicenseRoutes, { prefix: "/api/plugin/licenses" });
  await app.register(productRoutes, { prefix: "/v1/products" });
  await app.register(customerRoutes, { prefix: "/v1/customers" });
  await app.register(authRoutes, { prefix: "/v1/auth" });
  await app.register(checkoutRoutes, { prefix: "/v1/checkout" });
  await app.register(stripeWebhookRoutes, { prefix: "/v1/webhooks" });
  await app.register(licenseRoutes, { prefix: "/v1/licenses" });
  await app.register(meRoutes, { prefix: "/v1/me" });

  return app;
}

async function main() {
  const app = await buildApp();

  try {
    await app.listen({ port: env.PORT, host: "0.0.0.0" });
    app.log.info(`Server listening on port ${env.PORT}`);
  } catch (error) {
    app.log.error(error, "Failed to start server");
    process.exit(1);
  }

  const shutdown = async (signal: string) => {
    app.log.info({ signal }, "Shutting down server");
    await app.close();
    process.exit(0);
  };

  process.on("SIGTERM", () => {
    void shutdown("SIGTERM");
  });
  process.on("SIGINT", () => {
    void shutdown("SIGINT");
  });
}

// Compare filesystem paths (not a hand-built file:// URL) so the entry-point
// guard works on Windows too, where argv[1] contains backslashes.
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  void main();
}
