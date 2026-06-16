import { FastifyInstance } from "fastify";
import { requireAuth } from "../../plugins/auth.js";
import { CustomerService } from "../../services/CustomerService.js";
import { LicenseService } from "../../services/LicenseService.js";
import { prisma } from "../../db/client.js";
import { ApiError, ErrorCode } from "../../lib/errors.js";

export async function meRoutes(app: FastifyInstance) {
  app.get("/", async (request, reply) => {
    const customer = await requireAuth(request);
    const profile = await CustomerService.findById(customer.id);
    if (!profile) {
      throw new ApiError(ErrorCode.NOT_FOUND, "Customer not found");
    }

    const orders = await prisma.order.findMany({
      where: { customerId: customer.id },
      orderBy: { createdAt: "desc" },
      include: {
        product: { select: { slug: true, name: true } },
        licenses: { select: { id: true, key: true, status: true, createdAt: true } },
      },
    });

    const licenses = await LicenseService.findByCustomerId(customer.id);

    return reply.send({
      customer: profile,
      orders: orders.map((o) => ({
        id: o.id,
        status: o.status,
        amountCents: o.amountCents,
        currency: o.currency,
        createdAt: o.createdAt,
        paidAt: o.paidAt,
        product: o.product,
        licenses: o.licenses.map((l) => ({
          id: l.id,
          key: maskLicenseKey(l.key),
          status: l.status,
        })),
      })),
      licenses,
    });
  });
}

function maskLicenseKey(key: string): string {
  const parts = key.split("-");
  return parts.map((part, i) => (i === 0 ? part : "****")).join("-");
}
