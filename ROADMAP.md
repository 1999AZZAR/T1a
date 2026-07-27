# T1a — Roadmap & Status

> Status: Jul 2026 — OpenCode provider • Built-in MCP • Guardian memory
> Binary: ~120KB | RAM: ~10MiB | Deps: BearSSL only

## Overview

T1a (noclaw) is a pure-C AI agent for resource-constrained devices
(Luckfox Pico Mini, ESP32-class SBCs). Designed to run **one per device**,
each connecting directly to OpenCode's free API (`opencode.ai/zen/v1`).
No central LLM proxy, per-device rate limiting, true horizontal scaling.

### Architecture

```
main.c                               Entry point
 ├─ config.c                         JSON config + ENV → OpenCode
 ├─ agent.c                          Chat loop → LLM → tools → response
 │   ├─ provider.c                   OpenCode provider (dedicated)
 │   ├─ commands.c                   Tool registration + Telegram commands
 │   │   ├─ tools.c                  13 built-in tools
 │   │   ├─ mcp_builtin.c            reasoning + tavily_search + guardian_memory
 │   │   └─ mcp.c                    External MCP client (optional)
 │   └─ memory.c                     Guardian memory (persistent JSONL)
 ├─ channel.c / channel_cli.c        Telegram / CLI
 ├─ gateway.c                        HTTP gateway
 ├─ http.c                           BearSSL native TLS
 └─ json.c / arena.c / util.c        Parser, allocator, helpers
```

---

## ✅ Done

| # | Item | Commit | Notes |
|---|------|--------|-------|
| D1 | **OpenCode provider** | `9fa3c79` | Dedicated provider, removed Anthropic/chain |
| D2 | **ROADMAP.md** | `d105bf3` | Full source audit, 15-item plan |
| 4  | **Built-in reasoning** | `cd5b427` | Sequential thinking in `mcp_builtin.c` |
| 5  | **Built-in search** | `cd5b427` | Tavily direct HTTPS, no Node.js |
| 6  | **Guardian memory backend** | `bb40254` | Persistent JSONL, shared gm_ctx |
| 7  | **Caveman system prompt** | `cd5b427` | <200 token keyword-driven directive |
| —  | **Sliding window bump** | `c12d14a` | NC_MAX_MESSAGES 128→256, keep 75% |

**Files created:**
- `src/mcp_builtin.c` (3 built-in MCP tools, replaces external Node.js servers)

---

## OpenCode Models (all free, all $0)

| Model | API | Context | Notes |
|-------|-----|---------|-------|
| `deepseek-v4-flash-free` | OpenAI-compat | 1,000,000 | **Default.** Reasoning chain |
| `mimo-v2.5-free` | OpenAI-compat | 262,144 | Reasoning chain |
| `nemotron-3-ultra-free` | OpenAI-compat | 128,000 | Content in `content` field |
| `north-mini-code-free` | OpenAI-compat | 128,000 | Coding-focused |

Endpoint: `https://opencode.ai/zen/v1/chat/completions`

---

## To-Do (unprioritized)

### Tool Result Truncation
Truncate tool outputs >4KB with `[...truncated: N bytes]` note.
Prevents silent context overflow on small-window models.

### Streaming Reply
SSE streaming from provider, progressive messages to Telegram.
Keep typing indicator alive during long generation.

### Telegram Response Splitting
Split messages at 4096 chars, respect Markdown boundaries.

### MCP Client Resilience
Auto-restart dead servers, handle resource/image content types.

### Commands Expansion
`/memory`, `/model`, `/uptime`, `/caveman` commands for Telegram.

### Cost Tracking
Log tokens per call, prevent accidental paid-model usage.

### Gateway API
REST endpoints + WebSocket for external monitoring.

### Luckfox Deployment Guide
Cross-compilation, minimal Linux image, systemd unit, WiFi setup.
