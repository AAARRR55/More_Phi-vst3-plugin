import jwt from "jsonwebtoken";
import { createHash, randomBytes } from "crypto";
import { env } from "../config/env.js";

export interface AccessTokenPayload {
  sub: string; // customer id
  email: string;
  type: "access";
}

export interface RefreshTokenPayload {
  sub: string; // customer id
  jti: string; // token id
  type: "refresh";
}

export function signAccessToken(payload: Omit<AccessTokenPayload, "type">): string {
  return jwt.sign({ ...payload, type: "access" } as AccessTokenPayload, env.JWT_SECRET, {
    expiresIn: env.JWT_ACCESS_TTL,
  });
}

export function verifyAccessToken(token: string): AccessTokenPayload {
  return jwt.verify(token, env.JWT_SECRET, {
    algorithms: ["HS256"],
  }) as AccessTokenPayload;
}

export function generateRefreshToken(): { token: string; jti: string; hash: string } {
  const jti = randomBytes(32).toString("hex");
  const token = randomBytes(48).toString("base64url");
  const hash = createHash("sha256").update(token).digest("hex");
  return { token, jti, hash };
}

export function signRefreshToken(customerId: string, jti: string): string {
  return jwt.sign(
    { sub: customerId, jti, type: "refresh" } as RefreshTokenPayload,
    env.JWT_SECRET,
    {
      expiresIn: env.JWT_REFRESH_TTL,
    }
  );
}

export function verifyRefreshToken(token: string): RefreshTokenPayload {
  return jwt.verify(token, env.JWT_SECRET, {
    algorithms: ["HS256"],
  }) as RefreshTokenPayload;
}
