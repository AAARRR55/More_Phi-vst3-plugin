import { FastifyInstance } from "fastify";
import { prisma } from "../../db/client.js";

export async function healthRoutes(app: FastifyInstance) {
  app.get("/", async (_request, reply) => {
    let db = false;
    try {
      await prisma.$queryRaw`SELECT 1`;
      db = true;
    } catch {
      db = false;
    }

    const status = db ? 200 : 503;
    return reply.status(status).send({ status: db ? "ok" : "error", db });
  });
}
