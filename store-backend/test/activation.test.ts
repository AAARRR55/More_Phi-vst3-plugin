import { describe, it, expect } from "vitest";
import { buildTestApp, createCustomer, createOrder, createLicense } from "./helpers.js";
import { licenseKeyHash } from "../src/lib/crypto.js";
import { prisma } from "../src/db/client.js";

describe("License activation", () => {
  it("activates a license on a new machine and returns a signed certificate", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("activation-1@example.com");
    const order = await createOrder(customer.id);
    const { plainKey } = await createLicense(customer.id, order.id);

    const response = await app.inject({
      method: "POST",
      url: "/v1/licenses/activations",
      payload: {
        licenseKey: plainKey,
        machineFingerprint: "machine-a",
        hostname: "Studio-PC",
        os: "win32",
        appVersion: "3.3.0",
      },
    });

    expect(response.statusCode).toBe(200);
    const body = response.json<{
      status: string;
      activationId: string;
      certificate: { payload: string; signature: string; keyId: string };
    }>();
    expect(body.status).toBe("ACTIVE");
    expect(body.certificate.payload).toBeDefined();
    expect(body.certificate.signature).toBeDefined();
  });

  it("allows re-activation of the same machine without consuming a seat", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("activation-2@example.com");
    const order = await createOrder(customer.id);
    const { plainKey } = await createLicense(customer.id, order.id);

    await app.inject({
      method: "POST",
      url: "/v1/licenses/activations",
      payload: { licenseKey: plainKey, machineFingerprint: "machine-b" },
    });

    const second = await app.inject({
      method: "POST",
      url: "/v1/licenses/activations",
      payload: { licenseKey: plainKey, machineFingerprint: "machine-b" },
    });

    expect(second.statusCode).toBe(200);
    const activeCount = await prisma.activation.count({
      where: { license: { keyHash: licenseKeyHash(plainKey) }, status: "ACTIVE" },
    });
    expect(activeCount).toBe(1);
  });

  it("enforces the activation limit", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("activation-3@example.com");
    const order = await createOrder(customer.id);
    const { plainKey } = await createLicense(customer.id, order.id);

    for (let i = 0; i < 3; i++) {
      await app.inject({
        method: "POST",
        url: "/v1/licenses/activations",
        payload: { licenseKey: plainKey, machineFingerprint: `machine-${i}` },
      });
    }

    const overflow = await app.inject({
      method: "POST",
      url: "/v1/licenses/activations",
      payload: { licenseKey: plainKey, machineFingerprint: "machine-overflow" },
    });

    expect(overflow.statusCode).toBe(409);
  });

  it("deactivates a machine and frees a seat", async () => {
    const app = await buildTestApp();
    const customer = await createCustomer("activation-4@example.com");
    const order = await createOrder(customer.id);
    const { plainKey } = await createLicense(customer.id, order.id);

    await app.inject({
      method: "POST",
      url: "/v1/licenses/activations",
      payload: { licenseKey: plainKey, machineFingerprint: "machine-d" },
    });

    const deactivate = await app.inject({
      method: "POST",
      url: "/v1/licenses/activations/deactivate",
      payload: { licenseKey: plainKey, machineFingerprint: "machine-d" },
    });

    expect(deactivate.statusCode).toBe(200);
    const activeCount = await prisma.activation.count({
      where: { license: { keyHash: licenseKeyHash(plainKey) }, status: "ACTIVE" },
    });
    expect(activeCount).toBe(0);
  });
});
