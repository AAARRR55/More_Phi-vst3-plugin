import { FastifyInstance, FastifyError, FastifyRequest, FastifyReply } from "fastify";
import { ZodError } from "zod";
import { env } from "../config/env.js";
import { ApiError, ErrorCode, getErrorResponse } from "../lib/errors.js";
import { logger } from "../lib/logger.js";

export function registerErrorHandler(app: FastifyInstance) {
  app.setErrorHandler((error: FastifyError, _request: FastifyRequest, reply: FastifyReply) => {
    if (error instanceof ZodError) {
      return reply.status(422).send({
        error: {
          code: ErrorCode.VALIDATION_ERROR,
          message: "Validation failed",
          details: error.flatten(),
        },
      });
    }

    if (error instanceof ApiError) {
      return reply.status(error.statusCode).send(getErrorResponse(error));
    }

    if (error.statusCode === 429) {
      return reply.status(429).send({
        error: {
          code: ErrorCode.RATE_LIMITED,
          message: error.message || "Rate limit exceeded",
        },
      });
    }

    if (error.validation) {
      return reply.status(400).send({
        error: {
          code: ErrorCode.VALIDATION_ERROR,
          message: error.message,
          details: error.validation,
        },
      });
    }

    logger.error(
      {
        err: env.NODE_ENV === "production" ? error.message : error,
        code: error.code,
        statusCode: error.statusCode,
      },
      "Unhandled error"
    );

    return reply.status(error.statusCode ?? 500).send({
      error: {
        code: ErrorCode.INTERNAL_ERROR,
        message: "An unexpected error occurred",
        ...(env.NODE_ENV === "development" ? { details: error.message } : {}),
      },
    });
  });
}
