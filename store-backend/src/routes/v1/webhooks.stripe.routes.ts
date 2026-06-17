import { FastifyInstance } from "fastify";
import { StripeService } from "../../services/StripeService.js";
import { WebhookService } from "../../services/WebhookService.js";
import { logger } from "../../lib/logger.js";

export async function stripeWebhookRoutes(app: FastifyInstance) {
  app.post(
    "/stripe",
    {
      config: { rateLimit: false },
    },
    async (request, reply) => {
      const signature = request.headers["stripe-signature"];
      if (typeof signature !== "string") {
        return reply.status(400).send({ error: { code: "BAD_REQUEST", message: "Missing stripe-signature header" } });
      }

      const payload = request.rawBody;
      if (!Buffer.isBuffer(payload)) {
        return reply.status(400).send({ error: { code: "BAD_REQUEST", message: "Raw body required for signature verification" } });
      }

      const event = StripeService.constructWebhookEvent(payload, signature);
      const result = await WebhookService.handleEvent(event);

      logger.info({ stripeEventId: event.id, type: event.type, result }, "Stripe webhook processed");

      return reply.send({ received: true, processed: result.processed });
    }
  );
}
