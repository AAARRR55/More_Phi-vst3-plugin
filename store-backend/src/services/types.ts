import { Order, Product, Customer, License, Activation, PaymentEvent } from "@prisma/client";

export type OrderWithCustomerAndProduct = Order & {
  customer: Customer;
  product: Product;
  licenses: (License & { activations: Activation[] })[];
  paymentEvents: PaymentEvent[];
};
