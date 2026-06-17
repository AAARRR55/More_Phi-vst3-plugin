import pino from "pino";
import { env } from "../config/env.js";

export const logger = pino({
  level: env.LOG_LEVEL,
  base: {
    service: "more-phi-store-backend",
    version: "1.0.0",
  },
  redact: {
    paths: [
      "*.password",
      "*.passwordHash",
      "*.licenseKey",
      "*.authorization",
      "req.headers.authorization",
      "req.headers.cookie",
      "res.headers['set-cookie']",
    ],
    censor: "[REDACTED]",
    remove: false,
  },
  transport:
    env.NODE_ENV === "development"
      ? {
          target: "pino-pretty",
          options: {
            colorize: true,
            translateTime: "SYS:standard",
            ignore: "pid,hostname",
          },
        }
      : undefined,
});

export function getRequestLogger(requestId: string) {
  return logger.child({ requestId });
}
