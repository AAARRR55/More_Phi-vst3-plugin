import { prisma } from "./db/client.js";
import { LicenseService } from "./services/LicenseService.js";

async function main() {
  const order = await prisma.order.findFirst({
    where: { status: "PENDING" },
    orderBy: { createdAt: "desc" },
    include: { product: true }
  });

  if (!order) {
    console.log("No pending orders found.");
    return;
  }

  console.log(`Fulfilling order ${order.id} for customer ${order.customerId}...`);

  const paymentIntentId = "pi_mock_" + Math.random().toString(36).substring(7);

  const licenseResult = await prisma.$transaction(async (tx) => {
    await tx.order.update({
      where: { id: order.id },
      data: {
        status: "PAID",
        paidAt: new Date(),
        stripePaymentIntentId: paymentIntentId,
      },
    });

    return LicenseService.create(
      {
        customerId: order.customerId,
        orderId: order.id,
        productId: order.productId,
        maxActivations: order.product.maxActivations,
      },
      tx
    );
  });

  console.log("Order fulfilled successfully!");
  console.log(`License Key: ${licenseResult.plainKey}`);
  console.log(`Checkout Session ID: ${order.stripeCheckoutSessionId}`);
}

main()
  .catch((err) => {
    console.error(err);
    process.exit(1);
  })
  .finally(async () => {
    await prisma.$disconnect();
  });
