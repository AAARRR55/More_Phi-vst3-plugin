import { z } from "zod";
import dotenv from "dotenv";

dotenv.config();

const envSchema = z
  .object({
    NODE_ENV: z.enum(["development", "test", "production"]).default("development"),
    PORT: z.coerce.number().int().positive().default(4000),
    LOG_LEVEL: z.enum(["trace", "debug", "info", "warn", "error", "fatal"]).default("info"),

    DATABASE_URL: z.string().min(1),

    JWT_SECRET: z.string().min(32),
    JWT_ACCESS_TTL: z.coerce.number().int().positive().default(900),
    JWT_REFRESH_TTL: z.coerce.number().int().positive().default(2592000),
    BCRYPT_COST: z.coerce.number().int().min(10).max(15).default(12),

    STRIPE_SECRET_KEY: z.string().min(1),
    STRIPE_WEBHOOK_SECRET: z.string().min(1),
    STRIPE_SUCCESS_URL: z.string().min(1),
    STRIPE_CANCEL_URL: z.string().min(1),

    CORS_ORIGIN: z.string().min(1),
    RATE_LIMIT_MAX: z.coerce.number().int().positive().default(100),
    RATE_LIMIT_WINDOW_MS: z.coerce.number().int().positive().default(60000),
    LOGIN_RATE_LIMIT_MAX: z.coerce.number().int().positive().default(10),
    LOGIN_RATE_LIMIT_WINDOW_MS: z.coerce.number().int().positive().default(60000),
    CHECKOUT_RATE_LIMIT_MAX: z.coerce.number().int().positive().default(6),
    CHECKOUT_RATE_LIMIT_WINDOW_MS: z.coerce.number().int().positive().default(60000),
    ACTIVATION_RATE_LIMIT_MAX: z.coerce.number().int().positive().default(20),
    ACTIVATION_RATE_LIMIT_WINDOW_MS: z.coerce.number().int().positive().default(60000),

    LICENSE_KEY_PREFIX: z.string().min(1).default("MP"),
    LICENSE_MAX_ACTIVATIONS: z.coerce.number().int().positive().default(3),
    LICENSE_SIGNING_PRIVATE_KEY: z.string().min(1),
    LICENSE_SIGNING_KEY_ID: z.string().min(1).default("prod-ed25519-2026-01"),

    EMAIL_PROVIDER: z.enum(["console", "resend", "smtp"]).default("console"),
    EMAIL_FROM: z.string().email(),
    RESEND_API_KEY: z.string().optional(),
    SMTP_HOST: z.string().optional(),
    SMTP_PORT: z.coerce.number().int().positive().default(587),
    SMTP_USER: z.string().optional(),
    SMTP_PASS: z.string().optional(),

    DOWNLOAD_URL_BASE: z.string().min(1),
  })
  .superRefine((data, ctx) => {
    if (data.EMAIL_PROVIDER === "resend" && !data.RESEND_API_KEY) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ["RESEND_API_KEY"],
        message: "RESEND_API_KEY is required when EMAIL_PROVIDER=resend",
      });
    }
    if (data.EMAIL_PROVIDER === "smtp") {
      if (!data.SMTP_HOST) {
        ctx.addIssue({
          code: z.ZodIssueCode.custom,
          path: ["SMTP_HOST"],
          message: "SMTP_HOST is required when EMAIL_PROVIDER=smtp",
        });
      }
      if (!data.SMTP_USER || !data.SMTP_PASS) {
        ctx.addIssue({
          code: z.ZodIssueCode.custom,
          path: ["SMTP_USER", "SMTP_PASS"],
          message: "SMTP_USER and SMTP_PASS are required when EMAIL_PROVIDER=smtp",
        });
      }
    }
  });

const parsed = envSchema.safeParse(process.env);

if (!parsed.success) {
  const formatted = parsed.error.issues
    .map((issue) => `${issue.path.join(".")}: ${issue.message}`)
    .join("\n");
  throw new Error(`Environment validation failed:\n${formatted}`);
}

export const env = {
  ...parsed.data,
  get licenseSigningPrivateKey(): string {
    return parsed.data.LICENSE_SIGNING_PRIVATE_KEY.replace(/\\n/g, "\n").trim();
  },
};

export type Env = typeof env;
