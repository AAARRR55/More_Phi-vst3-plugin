import { describe, it, expect } from "vitest";
import { buildTestApp, createCustomer, login } from "./helpers.js";

/** Extract the `mp_refresh` cookie value from a fastify-inject response. */
function refreshCookieFrom(response: { cookies: { name: string; value: string }[] }): string {
  const cookie = response.cookies.find((c) => c.name === "mp_refresh");
  if (!cookie) throw new Error("mp_refresh cookie not present in response");
  return cookie.value;
}

describe("Auth routes", () => {
  it("registers a new customer", async () => {
    const app = await buildTestApp();
    const response = await app.inject({
      method: "POST",
      url: "/v1/customers",
      payload: {
        email: "auth-test-1@example.com",
        password: "Password123!",
        name: "Test User",
      },
    });

    expect(response.statusCode).toBe(201);
    const body = response.json<{ id: string; email: string }>();
    expect(body.email).toBe("auth-test-1@example.com");
  });

  it("rejects duplicate registration", async () => {
    const app = await buildTestApp();
    await createCustomer("auth-test-2@example.com");

    const response = await app.inject({
      method: "POST",
      url: "/v1/customers",
      payload: { email: "auth-test-2@example.com", password: "Password123!" },
    });

    expect(response.statusCode).toBe(409);
  });

  it("logs in with valid credentials", async () => {
    const app = await buildTestApp();
    await createCustomer("auth-test-3@example.com");

    const response = await app.inject({
      method: "POST",
      url: "/v1/auth/login",
      payload: { email: "auth-test-3@example.com", password: "Password123!" },
    });

    expect(response.statusCode).toBe(200);
    const body = response.json<{ accessToken: string }>();
    expect(body.accessToken).toBeDefined();
    expect(response.cookies).toEqual(
      expect.arrayContaining([
        expect.objectContaining({ name: "mp_auth" }),
        expect.objectContaining({ name: "mp_refresh" }),
      ])
    );
  });

  it("rejects invalid credentials", async () => {
    const app = await buildTestApp();
    await createCustomer("auth-test-4@example.com");

    const response = await app.inject({
      method: "POST",
      url: "/v1/auth/login",
      payload: { email: "auth-test-4@example.com", password: "WrongPassword" },
    });

    expect(response.statusCode).toBe(401);
  });

  it("returns profile for authenticated user", async () => {
    const app = await buildTestApp();
    await createCustomer("auth-test-5@example.com");
    const token = await login(app, "auth-test-5@example.com");

    const response = await app.inject({
      method: "GET",
      url: "/v1/me",
      headers: { authorization: `Bearer ${token}` },
    });

    expect(response.statusCode).toBe(200);
    const body = response.json<{ customer: { email: string } }>();
    expect(body.customer.email).toBe("auth-test-5@example.com");
  });

  it("rotation revokes the old refresh token", async () => {
    const app = await buildTestApp();
    await createCustomer("auth-test-rotate-1@example.com");

    // Login -> cookie A (the "old" refresh token).
    const loginRes = await app.inject({
      method: "POST",
      url: "/v1/auth/login",
      payload: { email: "auth-test-rotate-1@example.com", password: "Password123!" },
    });
    expect(loginRes.statusCode).toBe(200);
    const oldRefresh = refreshCookieFrom(loginRes);

    // Rotate once -> 200, new cookies (token B).
    const firstRefresh = await app.inject({
      method: "POST",
      url: "/v1/auth/refresh",
      cookies: { mp_refresh: oldRefresh },
    });
    expect(firstRefresh.statusCode).toBe(200);
    const newRefresh = refreshCookieFrom(firstRefresh);
    expect(newRefresh).not.toBe(oldRefresh);

    // The OLD token must no longer be usable -> 401.
    const replay = await app.inject({
      method: "POST",
      url: "/v1/auth/refresh",
      cookies: { mp_refresh: oldRefresh },
    });
    expect(replay.statusCode).toBe(401);
    const body = replay.json<{ error: { code: string; message: string } }>();
    expect(body.error.code).toBe("UNAUTHORIZED");
  });

  it("reuse of a revoked token revokes the customer's token family", async () => {
    const app = await buildTestApp();
    await createCustomer("auth-test-reuse-1@example.com");

    // Login -> token A.
    const loginRes = await app.inject({
      method: "POST",
      url: "/v1/auth/login",
      payload: { email: "auth-test-reuse-1@example.com", password: "Password123!" },
    });
    expect(loginRes.statusCode).toBe(200);
    const tokenA = refreshCookieFrom(loginRes);

    // Rotate A -> token B (A is revoked).
    const rotateRes = await app.inject({
      method: "POST",
      url: "/v1/auth/refresh",
      cookies: { mp_refresh: tokenA },
    });
    expect(rotateRes.statusCode).toBe(200);
    const tokenB = refreshCookieFrom(rotateRes);

    // Reuse the already-revoked A -> 401, and family-wide revocation fires.
    const reuseRes = await app.inject({
      method: "POST",
      url: "/v1/auth/refresh",
      cookies: { mp_refresh: tokenA },
    });
    expect(reuseRes.statusCode).toBe(401);

    // The legitimate new token B must now ALSO be invalid, because reusing A
    // burned the whole family -> forces a fresh login.
    const bAfterReuse = await app.inject({
      method: "POST",
      url: "/v1/auth/refresh",
      cookies: { mp_refresh: tokenB },
    });
    expect(bAfterReuse.statusCode).toBe(401);
  });
});
