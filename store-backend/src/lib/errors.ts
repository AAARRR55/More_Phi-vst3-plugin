export enum ErrorCode {
  VALIDATION_ERROR = "VALIDATION_ERROR",
  NOT_FOUND = "NOT_FOUND",
  UNAUTHORIZED = "UNAUTHORIZED",
  FORBIDDEN = "FORBIDDEN",
  CONFLICT = "CONFLICT",
  RATE_LIMITED = "RATE_LIMITED",
  INTERNAL_ERROR = "INTERNAL_ERROR",
  BAD_REQUEST = "BAD_REQUEST",
  PAYMENT_REQUIRED = "PAYMENT_REQUIRED",
  MAX_ACTIVATIONS_REACHED = "MAX_ACTIVATIONS_REACHED",
  LICENSE_REVOKED = "LICENSE_REVOKED",
  LICENSE_EXPIRED = "LICENSE_EXPIRED",
  WEBHOOD_INVALID_SIGNATURE = "WEBHOOD_INVALID_SIGNATURE",
}

const statusMap: Record<ErrorCode, number> = {
  [ErrorCode.VALIDATION_ERROR]: 422,
  [ErrorCode.NOT_FOUND]: 404,
  [ErrorCode.UNAUTHORIZED]: 401,
  [ErrorCode.FORBIDDEN]: 403,
  [ErrorCode.CONFLICT]: 409,
  [ErrorCode.RATE_LIMITED]: 429,
  [ErrorCode.INTERNAL_ERROR]: 500,
  [ErrorCode.BAD_REQUEST]: 400,
  [ErrorCode.PAYMENT_REQUIRED]: 402,
  [ErrorCode.MAX_ACTIVATIONS_REACHED]: 409,
  [ErrorCode.LICENSE_REVOKED]: 403,
  [ErrorCode.LICENSE_EXPIRED]: 403,
  [ErrorCode.WEBHOOD_INVALID_SIGNATURE]: 400,
};

export class ApiError extends Error {
  public readonly code: ErrorCode;
  public readonly statusCode: number;
  public readonly details?: Record<string, unknown>;

  constructor(code: ErrorCode, message: string, details?: Record<string, unknown>) {
    super(message);
    this.name = "ApiError";
    this.code = code;
    this.statusCode = statusMap[code] ?? 500;
    this.details = details;
  }
}

export function isApiError(error: unknown): error is ApiError {
  return error instanceof ApiError;
}

export function getErrorResponse(error: unknown) {
  if (isApiError(error)) {
    return {
      error: {
        code: error.code,
        message: error.message,
        details: error.details,
      },
    };
  }

  return {
    error: {
      code: ErrorCode.INTERNAL_ERROR,
      message: "An unexpected error occurred",
    },
  };
}
