import { FastifyInstance } from "fastify";
import { z } from "zod";
import { CustomerService } from "../../services/CustomerService.js";
import { validate } from "../../lib/validate.js";
import { env } from "../../config/env.js";

function optionalNonEmptyString(schema: z.ZodString) {
  return z.union([z.literal(""), schema]).transform((val) => (val === "" ? undefined : val)).optional();
}

const createCustomerSchema = z.object({
  email: z.string().email(),
  password: z.string().min(8).max(128),
  name: z.string().min(1).max(120).optional(),
  company: optionalNonEmptyString(z.string().max(120)),
  country: optionalNonEmptyString(z.string().length(2)),
});

export async function customerRoutes(app: FastifyInstance) {
  app.post(
    "/",
    {
      config: {
        rateLimit: {
          max: env.LOGIN_RATE_LIMIT_MAX,
          timeWindow: env.LOGIN_RATE_LIMIT_WINDOW_MS,
        },
      },
    },
    async (request, reply) => {
      const input = validate(createCustomerSchema, request.body);
      const customer = await CustomerService.create(input);
      return reply.status(201).send(customer);
    }
  );
}
