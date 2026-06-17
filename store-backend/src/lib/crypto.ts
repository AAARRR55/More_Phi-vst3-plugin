import { createHash, createPrivateKey, randomBytes, sign } from "crypto";
import { promisify } from "util";
import bcrypt from "bcrypt";
import { env } from "../config/env.js";

const CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const AMBIGUITY_STRIPPED_ALPHABET = "23456789ABCDEFGHJKMNPQRSTVWXYZ";
const CRC_TABLE = new Uint32Array(256);

for (let i = 0; i < 256; i++) {
  let crc = i;
  for (let j = 0; j < 8; j++) {
    crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  CRC_TABLE[i] = crc >>> 0;
}

export async function hashPassword(plain: string): Promise<string> {
  return bcrypt.hash(plain, env.BCRYPT_COST);
}

export async function verifyPassword(plain: string, hash: string): Promise<boolean> {
  return bcrypt.compare(plain, hash);
}

export function sha256(input: string): string {
  return createHash("sha256").update(input, "utf8").digest("hex");
}

export function randomBytesAsync(size: number): Promise<Buffer> {
  return promisify(randomBytes)(size);
}

function crc32Mod32(data: string): number {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc = CRC_TABLE[(crc ^ data.charCodeAt(i)) & 0xff] ^ (crc >>> 8);
  }
  return (~crc >>> 0) % 32;
}

function randomAmbiguityStrippedChar(): string {
  const byte = randomBytes(1)[0];
  return AMBIGUITY_STRIPPED_ALPHABET[byte % AMBIGUITY_STRIPPED_ALPHABET.length];
}
export function generateLicenseKey(): string {
  const bodyLength = 20; // 5 groups of 4 characters
  let body = "";
  for (let i = 0; i < bodyLength; i++) {
    body += randomAmbiguityStrippedChar();
  }

  const chunks = [];
  for (let i = 0; i < body.length; i += 4) {
    chunks.push(body.slice(i, i + 4));
  }

  // The plugin computes the checksum over the compact alphanumeric string
  // (prefix + body, no separators).
  const compact = `${env.LICENSE_KEY_PREFIX}${body}`;
  const checksumIndex = crc32Mod32(compact);
  const checksum = CROCKFORD_ALPHABET[checksumIndex];

  return `${env.LICENSE_KEY_PREFIX}-${chunks.join("-")}-${checksum}`;
}

export function normalizeLicenseKey(key: string): string {
  return key.toUpperCase().replace(/[^A-Z0-9]/g, "");
}

export function licenseKeyHash(key: string): string {
  return sha256(normalizeLicenseKey(key));
}

export function isValidLicenseKeyFormat(key: string): boolean {
  const normalized = normalizeLicenseKey(key);
  if (normalized.length < 12) return false;
  const prefix = env.LICENSE_KEY_PREFIX;
  if (!normalized.startsWith(prefix)) return false;
  const bodyWithChecksum = normalized.slice(prefix.length);
  // MPH1: 20 body chars (5 groups of 4) + 1 checksum char
  if (bodyWithChecksum.length !== 21) return false;
  const checksumChar = bodyWithChecksum.slice(-1);
  const compact = normalized.slice(0, -1);
  const expectedIndex = crc32Mod32(compact);
  return CROCKFORD_ALPHABET[expectedIndex] === checksumChar;
}

function canonicalJson(value: unknown): Buffer {
  const replacer = (_key: string, val: unknown): unknown => {
    if (val && typeof val === "object" && !Array.isArray(val)) {
      const sorted: Record<string, unknown> = {};
      for (const k of Object.keys(val).sort()) {
        sorted[k] = (val as Record<string, unknown>)[k];
      }
      return sorted;
    }
    return val;
  };
  return Buffer.from(JSON.stringify(value, replacer), "utf8");
}

export const PRODUCT_PUBLIC_ID = "more-phi-vst3";

export interface LicenseCertificate {
  schema: string;
  licenseId: string;
  activationId: string;
  productId: string;
  licenseType: "perpetual";
  features: string[];
  issuedAtUnix: number;
  validFromUnix: number;
  validUntilUnix: null;
  trialEndsAtUnix: null;
  subscriptionEndsAtUnix: null;
  leaseEndsAtUnix: null;
  nextOnlineCheckAtUnix: number;
  offlineGraceEndsAtUnix: number;
  machineHash: string;
  nonce: string;
}

export interface SignedActivationCertificate {
  certificatePayload: string;
  signature: string;
  publicKeyId: string;
}

export function signActivationCertificate(
  certificate: LicenseCertificate
): SignedActivationCertificate {
  const payloadBytes = canonicalJson(certificate);
  const certificatePayload = payloadBytes.toString("base64url").replace(/=+$/, "");

  // Development bypass: the plugin trusts the hardcoded "dev-key" / "dev-signature" pair.
  // The signature value itself must be valid base64url so the plugin can decode it before
  // comparing against the trusted development signature string.
  if (env.LICENSE_SIGNING_KEY_ID === "dev-key") {
    return {
      certificatePayload,
      signature: Buffer.from("dev-signature").toString("base64url").replace(/=+$/, ""),
      publicKeyId: "dev-key",
    };
  }

  const privateKeyPem = env.licenseSigningPrivateKey;
  const privateKey = createPrivateKey(privateKeyPem);
  const signature = sign(null, payloadBytes, privateKey);

  return {
    certificatePayload,
    signature: signature.toString("base64url").replace(/=+$/, ""),
    publicKeyId: env.LICENSE_SIGNING_KEY_ID,
  };
}

export function createCertificate(
  licenseId: string,
  activationId: string,
  productId: string,
  machineHash: string
): LicenseCertificate {
  const now = new Date();
  const nextCheck = new Date(now.getTime() + 30 * 24 * 60 * 60 * 1000);
  const graceUntil = new Date(nextCheck.getTime() + 14 * 24 * 60 * 60 * 1000);

  return {
    schema: "morephi-license-cert-v1",
    licenseId,
    activationId,
    productId,
    licenseType: "perpetual",
    features: [
      "morph_pad",
      "plugin_hosting",
      "mcp",
      "premium_presets",
      "audio_domain_morphing",
    ],
    issuedAtUnix: Math.floor(now.getTime() / 1000),
    validFromUnix: Math.floor(now.getTime() / 1000),
    validUntilUnix: null,
    trialEndsAtUnix: null,
    subscriptionEndsAtUnix: null,
    leaseEndsAtUnix: null,
    nextOnlineCheckAtUnix: Math.floor(nextCheck.getTime() / 1000),
    offlineGraceEndsAtUnix: Math.floor(graceUntil.getTime() / 1000),
    machineHash,
    nonce: randomBytes(16).toString("base64url").replace(/=+$/, ""),
  };
}
