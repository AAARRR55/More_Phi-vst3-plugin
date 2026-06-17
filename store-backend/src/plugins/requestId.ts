import { FastifyInstance } from "fastify";
import { randomUUID } from "crypto";

export function registerRequestId(app: FastifyInstance) {
  app.addHook("onRequest", (request, reply, done) => {
    const requestId = request.headers["x-request-id"]?.toString() ?? randomUUID();
    request.id = requestId;
    void reply.header("x-request-id", requestId);
    done();
  });
}
