import { FastifyInstance } from "fastify";
import helmet from "@fastify/helmet";

export async function registerHelmet(app: FastifyInstance) {
  await app.register(helmet, {
    contentSecurityPolicy: {
      directives: {
        defaultSrc: ["'self'"],
        styleSrc: ["'self'", "'unsafe-inline'"],
        scriptSrc: ["'self'"],
        imgSrc: ["'self'", "data:", "https:"],
      },
    },
  });
}
