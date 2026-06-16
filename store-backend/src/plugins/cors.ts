import { FastifyInstance } from "fastify";
import cors from "@fastify/cors";
import { env } from "../config/env.js";

export async function registerCors(app: FastifyInstance) {
  const allowedOrigins = env.CORS_ORIGIN.split(",").map((o) => o.trim());

  await app.register(cors, {
    origin: (origin, cb) => {
      if (!origin) {
        cb(null, true);
        return;
      }
      if (allowedOrigins.includes(origin) || allowedOrigins.includes("*")) {
        cb(null, true);
        return;
      }
      cb(new Error("Not allowed by CORS"), false);
    },
    credentials: true,
    methods: ["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"],
    allowedHeaders: ["Content-Type", "Authorization", "X-Request-Id"],
  });
}
