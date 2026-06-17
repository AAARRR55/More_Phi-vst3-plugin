import { FastifyInstance } from "fastify";
import cors from "@fastify/cors";
import { env } from "../config/env.js";

export async function registerCors(app: FastifyInstance) {
  const configuredOrigins = env.CORS_ORIGIN.split(",").map((o) => o.trim());
  // Ensure common dev origins are always allowed so the browser can reach the API
  // whether the user navigates to localhost or 127.0.0.1.
  const devOrigins = ["http://localhost:3000", "http://127.0.0.1:3000"];
  const allowedOrigins = [...new Set([...configuredOrigins, ...devOrigins])];

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
