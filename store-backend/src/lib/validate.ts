import { ZodSchema, ZodError } from "zod";
import { ApiError, ErrorCode } from "./errors.js";

export function validate<T>(schema: ZodSchema<T>, value: unknown): T {
  const result = schema.safeParse(value);
  if (!result.success) {
    throw new ApiError(
      ErrorCode.VALIDATION_ERROR,
      "Validation failed",
      result.error.flatten()
    );
  }
  return result.data;
}

export function isZodError(error: unknown): error is ZodError {
  return error instanceof ZodError;
}
