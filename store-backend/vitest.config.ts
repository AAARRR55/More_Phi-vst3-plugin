import { defineConfig } from "vitest/config";
import dotenv from "dotenv";

dotenv.config({ path: ".env.test" });

export default defineConfig({
  test: {
    globals: true,
    environment: "node",
    setupFiles: ["./test/setup.ts"],
    // Integration tests hit a real Postgres DB (truncate/seed in hooks,
    // per-test app build + DB round-trips). The default 5s is too tight on a
    // cold DB or slower host, causing spurious "Test timed out in 5000ms"
    // failures. Raise both test and hook ceilings so DB-bound setup/teardown
    // (beforeAll truncate-all, beforeEach per-table truncate) cannot trip the
    // default timer. Business-logic assertions are unaffected.
    testTimeout: 30000,
    hookTimeout: 30000,
    fileParallelism: false,
    poolOptions: {
      threads: {
        singleThread: true,
      },
    },
  },
});
