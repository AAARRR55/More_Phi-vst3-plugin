import { describe, it, expect } from "vitest";
import { buildTestApp, createCustomer, login } from "./helpers.js";

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
});
