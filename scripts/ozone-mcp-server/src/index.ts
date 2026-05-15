// ozone-mcp-server/src/index.ts
//
// MCP server that exposes iZotope Ozone Track Assistant capabilities
// as callable tools for AI agents (Claude, GPT-4, custom LLMs).
//
// Transport:
//   stdio  — default; suitable for local agent processes
//   HTTP+SSE — see Section 4.4 below; swap main() for httpMain()
//
// Configuration (environment variables — never hardcode):
//   OZONE_API_KEY    — Bearer token for Ozone Track Assistant API
//   OZONE_BASE_URL   — Base URL, e.g. https://internal.ozone.example.com/api/v1
//   MCP_AUTH_SECRET  — (HTTP mode only) validates incoming MCP client identity
//   MCP_PORT         — (HTTP mode only) listening port, defaults to 3000

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
import axios, { AxiosInstance } from "axios";
import axiosRetry from "axios-retry";

// ── Configuration ────────────────────────────────────────────────────────────

const OZONE_API_KEY  = process.env.OZONE_API_KEY;
const OZONE_BASE_URL = process.env.OZONE_BASE_URL;

if (!OZONE_API_KEY || !OZONE_BASE_URL) {
  console.error(
    "FATAL: OZONE_API_KEY and OZONE_BASE_URL environment variables must be set.\n" +
    "Copy scripts/ozone-mcp-server/.env.example to .env and populate the values."
  );
  process.exit(1);
}

// ── Authenticated Ozone HTTP Client ──────────────────────────────────────────

const ozoneClient: AxiosInstance = axios.create({
  baseURL: OZONE_BASE_URL,
  timeout: 15_000,
  headers: {
    Authorization: `Bearer ${OZONE_API_KEY}`,
    "Content-Type": "application/json",
    "X-Client-Name": "ozone-mcp-server/1.0.0",
  },
});

// Exponential backoff: 3 retries on network errors or HTTP 429
axiosRetry(ozoneClient, {
  retries: 3,
  retryDelay: axiosRetry.exponentialDelay,
  retryCondition: (err) =>
    axiosRetry.isNetworkOrIdempotentRequestError(err) ||
    err.response?.status === 429,
});

function normalizeOzoneError(err: unknown): string {
  if (axios.isAxiosError(err)) {
    const status = err.response?.status;
    const msg    = (err.response?.data as { message?: string })?.message ?? err.message;
    return `Ozone API error (HTTP ${status}): ${msg}`;
  }
  return `Unexpected error: ${String(err)}`;
}

// ── MCP Server Setup ─────────────────────────────────────────────────────────

const server = new Server(
  { name: "ozone-track-assistant-mcp", version: "1.0.0" },
  { capabilities: { tools: {} } }
);

// ── Tool Definitions (tools/list) ─────────────────────────────────────────────

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: "ozone_track_get_info",
      description:
        "Retrieve complete metadata and current status for a specific track in the Ozone " +
        "Track Assistant system. Returns track title, artist, current processing status, " +
        "mastering parameters, and any diagnostic notes.",
      inputSchema: {
        type: "object",
        properties: {
          track_id: {
            type: "string",
            description: "Unique Ozone track ID (format: trk_<20 alphanumeric chars>).",
            pattern: "^trk_[A-Za-z0-9]{20}$",
          },
          include_history: {
            type: "boolean",
            description: "Include full status-change history. Defaults to false.",
            default: false,
          },
        },
        required: ["track_id"],
      },
    },
    {
      name: "ozone_track_update_status",
      description:
        "Update the processing or review status of a track in the Ozone Track Assistant. " +
        "Valid statuses: pending_review, in_mastering, mastering_complete, approved, rejected, on_hold. " +
        "A 'reason' is required when setting status to 'rejected' or 'on_hold'.",
      inputSchema: {
        type: "object",
        properties: {
          track_id: {
            type: "string",
            description: "Unique Ozone track ID.",
            pattern: "^trk_[A-Za-z0-9]{20}$",
          },
          new_status: {
            type: "string",
            enum: [
              "pending_review",
              "in_mastering",
              "mastering_complete",
              "approved",
              "rejected",
              "on_hold",
            ],
            description: "Target status to transition this track to.",
          },
          reason: {
            type: "string",
            description:
              "Human-readable reason for the status change. Required for 'rejected' or 'on_hold'.",
            maxLength: 500,
          },
        },
        required: ["track_id", "new_status"],
      },
    },
    {
      name: "ozone_track_analyze",
      description:
        "Trigger an audio analysis job on a track, or retrieve the latest analysis report. " +
        "Returns spectral data, loudness metrics (LUFS integrated/short-term, true peak dBTP), " +
        "dynamic range, frequency balance, and Ozone's mastering assistant recommendations.",
      inputSchema: {
        type: "object",
        properties: {
          track_id: {
            type: "string",
            description: "Unique Ozone track ID.",
            pattern: "^trk_[A-Za-z0-9]{20}$",
          },
          force_reanalyze: {
            type: "boolean",
            description: "Discard cached analysis and run a fresh job. Defaults to false.",
            default: false,
          },
          analysis_profile: {
            type: "string",
            enum: ["standard", "streaming", "vinyl_master", "broadcast"],
            description: "Target loudness and mastering profile. Defaults to 'standard'.",
            default: "standard",
          },
        },
        required: ["track_id"],
      },
    },
    {
      name: "ozone_track_search",
      description:
        "Search the Track Assistant library by title, artist, status, or date range. " +
        "Returns a paginated list of matching tracks with summary metadata.",
      inputSchema: {
        type: "object",
        properties: {
          query: {
            type: "string",
            description: "Free-text search matched against track title and artist name.",
            maxLength: 200,
          },
          status_filter: {
            type: "array",
            items: {
              type: "string",
              enum: [
                "pending_review",
                "in_mastering",
                "mastering_complete",
                "approved",
                "rejected",
                "on_hold",
              ],
            },
            description: "Restrict results to these statuses. Omit for all statuses.",
          },
          date_from: {
            type: "string",
            format: "date",
            description: "ISO 8601 date (YYYY-MM-DD). Tracks created on or after this date.",
          },
          date_to: {
            type: "string",
            format: "date",
            description: "ISO 8601 date (YYYY-MM-DD). Tracks created on or before this date.",
          },
          page: {
            type: "integer",
            description: "Pagination page number (1-indexed). Defaults to 1.",
            minimum: 1,
            default: 1,
          },
          page_size: {
            type: "integer",
            description: "Number of results per page. Max 50. Defaults to 20.",
            minimum: 1,
            maximum: 50,
            default: 20,
          },
        },
        required: [],
      },
    },
  ],
}));

// ── Tool Call Handler (tools/call) ────────────────────────────────────────────

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {

      // ── ozone_track_get_info ──────────────────────────────────────────────
      case "ozone_track_get_info": {
        const { track_id, include_history = false } = args as {
          track_id: string;
          include_history?: boolean;
        };

        const resp = await ozoneClient.get(`/tracks/${track_id}`, {
          params: { include_history },
        });

        return {
          content: [{ type: "text", text: JSON.stringify(resp.data, null, 2) }],
          isError: false,
        };
      }

      // ── ozone_track_update_status ─────────────────────────────────────────
      case "ozone_track_update_status": {
        const { track_id, new_status, reason } = args as {
          track_id:   string;
          new_status: string;
          reason?:    string;
        };

        if (["rejected", "on_hold"].includes(new_status) && !reason) {
          return {
            content: [{
              type: "text",
              text: `A 'reason' is required when setting status to '${new_status}'.`,
            }],
            isError: true,
          };
        }

        const resp = await ozoneClient.patch(`/tracks/${track_id}/status`, {
          status: new_status,
          ...(reason && { reason }),
        });

        return {
          content: [{ type: "text", text: JSON.stringify(resp.data, null, 2) }],
          isError: false,
        };
      }

      // ── ozone_track_analyze ───────────────────────────────────────────────
      case "ozone_track_analyze": {
        const {
          track_id,
          force_reanalyze  = false,
          analysis_profile = "standard",
        } = args as {
          track_id:          string;
          force_reanalyze?:  boolean;
          analysis_profile?: string;
        };

        if (force_reanalyze) {
          const resp = await ozoneClient.post(`/tracks/${track_id}/analyze`, {
            profile: analysis_profile,
          });
          return {
            content: [{ type: "text", text: JSON.stringify(resp.data, null, 2) }],
            isError: false,
          };
        } else {
          const resp = await ozoneClient.get(`/tracks/${track_id}/analysis`, {
            params: { profile: analysis_profile },
          });
          return {
            content: [{ type: "text", text: JSON.stringify(resp.data, null, 2) }],
            isError: false,
          };
        }
      }

      // ── ozone_track_search ────────────────────────────────────────────────
      case "ozone_track_search": {
        const {
          query,
          status_filter,
          date_from,
          date_to,
          page      = 1,
          page_size = 20,
        } = args as {
          query?:         string;
          status_filter?: string[];
          date_from?:     string;
          date_to?:       string;
          page?:          number;
          page_size?:     number;
        };

        const resp = await ozoneClient.get("/tracks/search", {
          params: {
            ...(query                   && { q: query }),
            ...(status_filter?.length   && { status: status_filter.join(",") }),
            ...(date_from               && { created_after:  date_from }),
            ...(date_to                 && { created_before: date_to }),
            page,
            per_page: page_size,
          },
        });

        return {
          content: [{ type: "text", text: JSON.stringify(resp.data, null, 2) }],
          isError: false,
        };
      }

      default:
        return {
          content: [{ type: "text", text: `Unknown tool: ${name}` }],
          isError: true,
        };
    }
  } catch (err) {
    return {
      content: [{ type: "text", text: normalizeOzoneError(err) }],
      isError: true,
    };
  }
});

// ── stdio Transport (default) ─────────────────────────────────────────────────

async function main(): Promise<void> {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("[ozone-mcp-server] Listening on stdio");
}

main().catch((err) => {
  console.error("[ozone-mcp-server] Fatal:", err);
  process.exit(1);
});

// ── HTTP + SSE Transport (multi-agent, remote deployments) ───────────────────
//
// To use HTTP+SSE instead of stdio:
//   1. Comment out the main() call above
//   2. Uncomment httpMain() below and call it
//   3. Set MCP_PORT and MCP_AUTH_SECRET env vars
//
// import { SSEServerTransport } from "@modelcontextprotocol/sdk/server/sse.js";
// import express from "express";
//
// async function httpMain(): Promise<void> {
//   const app  = express();
//   const port = parseInt(process.env.MCP_PORT ?? "3000", 10);
//   const authSecret = process.env.MCP_AUTH_SECRET;
//
//   // Validate MCP client identity
//   app.use("/mcp", (req, res, next) => {
//     if (authSecret && req.headers["x-mcp-secret"] !== authSecret) {
//       res.status(401).json({ error: "Unauthorized MCP client" });
//       return;
//     }
//     next();
//   });
//
//   app.get("/mcp/sse", async (req, res) => {
//     const transport = new SSEServerTransport("/mcp/message", res);
//     await server.connect(transport);
//   });
//
//   app.post("/mcp/message", express.json(), async (_req, res) => {
//     res.status(202).end();
//   });
//
//   app.listen(port, () =>
//     console.error(`[ozone-mcp-server] HTTP+SSE on :${port}`)
//   );
// }
