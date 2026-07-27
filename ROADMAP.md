# T1a — Roadmap & Status

> Status: Jul 2026 — Sprint 1 complete, 7 commits
> Binary: 124KB | RAM: ~10MiB | Deps: BearSSL only
> API keys: Telegram token + Tavily (free, embedded)

## Overview

T1a is a pure-C AI agent for embedded devices (Luckfox Pico Mini, ESP32-class SBCs).
Runs one-per-device, each connecting directly to OpenCode's free API.
No central LLM proxy, per-device rate limiting, true horizontal scaling.

### Architecture

```
build.sh                 Interactive setup: keys → build → config
run_t1a.sh               Launcher with auto-restart

src/main.c               Entry: agent / gateway / status / doctor
src/config.c             JSON config + ENV → OpenCode
src/agent.c              Chat loop + caveman system prompt
src/provider.c           OpenCode provider (dedicated)
src/commands.c           Tool registration + Telegram commands
src/tools.c              13 built-in tools
src/mcp_builtin.c        4 built-in MCP tools (C, no Node.js)
src/mcp.c                External MCP client (optional)
src/memory.c             Guardian memory (persistent JSONL)
src/channel.c            Telegram long-poll channel
src/gateway.c            HTTP gateway
src/http.c               BearSSL native TLS
src/json.c / arena.c     Parser + allocator
```

---

## ✅ Done (Sprint 1 — July 2026)

| # | Item | Commit | What |
|---|------|--------|------|
| 1 | **OpenCode provider** | `9fa3c79` | Removed Anthropic/chain, dedicated provider |
| 2 | **Reasoning model fix** | `4507198` | Fallback to `reasoning_content` field |
| 3 | **Built-in reasoning MCP** | `cd5b427` | sequentialthinking in C |
| 4 | **Built-in search MCP** | `cd5b427` | tavily_search direct HTTPS, no Node.js |
| 5 | **Caveman system prompt** | `cd5b427` | <200 token keyword-driven directive |
| 6 | **Guardian memory backend** | `bb40254` | Persistent JSONL, shared gm_ctx |
| 7 | **Sliding window bump** | `c12d14a` | NC_MAX_MESSAGES 128→256, keep 75% |
| 8 | **Docs cleanup** | `bcf8814` | Removed ACP_PLAN.md, rewrote ROADMAP + README |
| 9 | **Wikipedia search tool** | `182a727` | Wikipedia Action API, free, no key needed |
| 10 | **build.sh interactive setup** | `9e35c9c` | Clone → ./build.sh → done |
| 11 | **README update** | `14e988c` | Quick start, systemd, features |
| 12 | **Prebuilt identity files** | `a8747d2` | Auto-gen SOUL.md, USER.md, IDENTITY.md |

**Files created:**
- `src/mcp_builtin.c` — 4 built-in MCP tools
- `build.sh` — interactive setup script
- `run_t1a.sh` — launcher with auto-restart

---

## OpenCode — Free Models

| Model | Context | Notes |
|-------|---------|-------|
| `deepseek-v4-flash-free` | 1,000,000 | **Default.** Reasoning chain |
| `mimo-v2.5-free` | 262,144 | Reasoning chain |
| `nemotron-3-ultra-free` | 128,000 | Content in `content` field |
| `north-mini-code-free` | 128,000 | Coding-focused |

Endpoint: `https://opencode.ai/zen/v1/chat/completions`

---

## Built-in MCP Tools (4)

| Tool | Source | API Key | Notes |
|------|--------|---------|-------|
| `sequentialthinking` | Pure C | None | Reasoning chain with branching |
| `tavily_search` | BearSSL HTTP | Tavily (free, embedded) | Web search |
| `wikipedia_search` | BearSSL HTTP | None | Encyclopedia, free |
| `guardian_memory` | Shared backend | None | Persistent entity store |

---

## Backlog (unprioritized)

### Tool Result Truncation
Truncate shell/HTTP/file outputs >4KB. Prevents silent context
overflow on small-window models. Low effort, high impact.

### Streaming Reply
SSE streaming from provider → progressive Telegram messages.
Keep typing indicator alive during generation. Medium effort.

### Telegram Response Splitting
Split at 4096 chars, respect Markdown boundaries. Low effort.

### MCP Client Resilience
Auto-restart dead external servers, handle resource/image content.
Low-medium effort.

### Commands Expansion
Add `/memory`, `/model`, `/uptime`, `/caveman` Telegram commands.
Low effort.

### Luckfox Deployment Guide
Cross-compile for Cortex-A7, Buildroot/Alpine image, systemd service,
WiFi config, power monitoring. Medium effort.

### Self-Test Suite
Expand existing `#ifdef NC_TEST` for provider mock, channel parsing,
rate limiter tests. Low effort.

### Cost Tracking
Prevent accidental paid-model usage. Log tokens per call.
Low effort.
