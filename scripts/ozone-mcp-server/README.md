# Ozone Track Assistant MCP Server

A Node.js/TypeScript MCP server that exposes iZotope Ozone Track Assistant capabilities as AI-callable tools.

## Architecture

```
AI Agent (Claude/GPT-4/custom LLM)
    │  JSON-RPC 2.0 (stdio or HTTP+SSE)
    ▼
ozone-mcp-server  ←── this package
    │  HTTPS + Bearer token
    ▼
Ozone Track Assistant REST API
```

## Tools Exposed

| Tool | Description |
|------|-------------|
| `ozone_track_get_info` | Retrieve track metadata, status, mastering params |
| `ozone_track_update_status` | Transition track through workflow states |
| `ozone_track_analyze` | Trigger/retrieve LUFS, dBTP, DR analysis |
| `ozone_track_search` | Search library by title, status, date range |

## Setup

```bash
cd scripts/ozone-mcp-server
npm install
npm run build
```

## Running

```bash
# stdio transport (default — for local AI agents)
OZONE_API_KEY=ozk_prod_xxx \
OZONE_BASE_URL=https://your-ozone-api/api/v1 \
node dist/index.js

# Then use the Python agent client for a demo workflow:
cd ../..
OZONE_API_KEY=... OZONE_BASE_URL=... python scripts/agent_client.py
```

## HTTP+SSE Transport

For multi-agent remote deployment, uncomment the `httpMain()` block in `src/index.ts` and set:

```bash
MCP_PORT=3000
MCP_AUTH_SECRET=your-shared-secret
```

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `OZONE_API_KEY` | ✅ | Bearer token for Ozone API |
| `OZONE_BASE_URL` | ✅ | API base URL |
| `MCP_AUTH_SECRET` | HTTP only | Validates incoming MCP clients |
| `MCP_PORT` | HTTP only | Listening port (default: 3000) |

Copy `.env.example` → `.env` and populate values. **Never commit `.env`.**
