# T1a — Roadmap & Findings

> Based on full source audit (Jul 2026) — 17 C source files, ~12K LOC
> Binary: ~150KB | RAM: ~10MiB | Dependencies: BearSSL (native TLS)

## Overview

T1a (noclaw) is already remarkably complete for a pure-C AI agent. It has:
a working agent loop, dual-provider support (OpenAI + Anthropic), full MCP
client, flat-file memory, Telegram/CLI channels, 13+ built-in tools, and
a nascent ACP delegation client. The gaps are in context management,
streaming, free-provider integration, and robustness.

---

## P0 — Critical (blocks usability on SBC / free endpoints)

### 1. Context Management — Summarization Before Trim

**Current:** When `NC_MAX_MESSAGES` (128) is hit, the agent keeps the last
64 messages + system prompt. Old messages are dropped silently, losing
conversation thread. (`agent.c:agent_push_msg`)

**Needed:**
- Before trimming, ask the LLM for a short summary of dropped messages
- Store that summary in memory (`memory_store`) under a key like
  `conv_summary_<timestamp>`
- Inject the summary as a synthetic "system" message after trimming
- Make the summary token-aware (count actual tokens, not message count)

**Why this matters for free endpoints:** Free models (GitHub Copilot, OpenCode)
have small context windows (4K-8K tokens). Without summarization, long
conversations instantly overflow the window, causing truncation or crashes.

**Implementation approach:**
- In `agent_push_msg`, when `message_count >= NC_MAX_MESSAGES`:
  1. Set `keep = NC_MAX_MESSAGES / 2` (existing logic)
  2. Before `memmove`, build a "Please summarize..." prompt from dropped msgs
  3. Call the LLM (agent->provider->chat) with a cheap model
  4. Store result in memory via agent->memory->store
  5. Insert as system message after compaction

**Files:** `src/agent.c`, `src/nc.h`

### 2. Streaming Reply Flow

**Current:** The provider sends the full response at once. On Telegram,
T1a sets typing indicator then sends one big message. (`channel.c`)

**Needed:**
- SSE/streaming support in the OpenAI provider (`provider.c:openai_chat`)
- Process `stream: true` in the request body
- Parse `data: {...}` SSE chunks as they arrive
- For Telegram: send partial messages progressively, or at minimum
  keep the typing indicator alive during generation
- Handle stream interruptions (HTTP disconnect, model timeout)

**Why this matters:** Synchronous responses >30s timeout on weak models.
Without streaming, users see T1a as "dead" or "stuck" during long
generations. On Luckfox-class hardware with free endpoints, latency
can hit 60+ seconds.

**Implementation approach:**
- Add `stream` field to `nc_chat_request`
- In `openai_chat`, when `req->stream`, send `"stream":true` in body
- Parse lines starting with `data: ` from the SSE response
- Use a callback or buffer to deliver tokens incrementally
- For Telegram: use `sendChatAction("typing")` every 5s while streaming

**Files:** `src/provider.c`, `src/nc.h`, `src/channel.c`

### 3. Free Provider Support — GitHub Copilot / OpenCode

**Current:** Only supports standard OpenAI-compatible API + Anthropic.
Requires paid API keys. (`provider.c`)

**Needed:**
- Adapter for GitHub Copilot Chat API (undocumented but used by copilot.vim)
- Adapter for OpenCode/OpenClaw free endpoints (OpenRouter free tier)
- Model routing: use free model for simple queries, paid for complex ones
- Cache successful responses to reduce API calls

**Why this matters:** Azzar's goal is 100% free operation. Without this,
T1a always needs paid API keys.

**Implementation approach:**
- GitHub Copilot uses `github.com/github-copilot/chat/completions`
  with an OAuth token from `gh auth token`
- Add `nc_provider_copilot()` that uses the gh token
- Add a model router in `nc_chat_request` that auto-selects based on
  prompt complexity (message count, tool count, etc.)
- Cache: store request/response pairs in flat-file memory

**Files:** `src/provider.c` (new provider function)

---

## P1 — High (improves reliability and capability)

### 4. Complete ACP Client Implementation

**Current:** `acp_client.c` has basic subprocess spawning and a
rudimentary session delegation pattern, but incomplete handshake.
It can spawn `codex` or `gemini` but the delegation flow is broken.

**Needed:**
- Full ACP `initialize` handshake (defined in `ACP_PLAN.md`)
- `session/new` and `session/prompt` JSON-RPC calls
- Non-blocking `select()` loop for `session/update` notifications
- Stream sub-agent output to active `nc_channel` in real-time
- Permission interception: auto-approve read-only ops, deny writes
- Graceful cleanup: SIGTERM on timeout, reap zombies via `waitpid`

**Why this matters:** ACP turns T1a from a standalone agent into a
**orchestrator** that can delegate complex tasks to codex/gemini
while T1a itself stays minimal. This is the key to running a lightweight
controller on resource-constrained devices.

**Files:** `src/acp_client.c`, `src/nc.h` (new ACP types)

### 5. Telegram Response Splitting (4096 char limit)

**Current:** `tg_send_msg` sends response as a single message. If
response > 4096 chars, Telegram silently truncates. (`channel.c`)

**Needed:**
- Split long messages into chunks of 4096 bytes (Telegram hard limit)
- If splitting mid-paragraph, find last sentence break
- Send chunks sequentially with `sendChatAction` between them
- Handle Markdown formatting correctly across chunk boundaries
  (don't split inside `**bold**` or inline code)

**Files:** `src/channel.c`

### 6. Tool Result Size Limit & Truncation

**Current:** Tool results are stored in a fixed 256KB buffer and sent
back to the LLM verbatim. Large outputs (shell, http_fetch, file_read)
can easily exceed the LLM's context window. (`agent.c`, `tools.c`)

**Needed:**
- After each tool execution, check result length
- If result > 4KB (configurable), truncate with a summary note:
  `"... [truncated: 128KB total, showing first 4KB]"`
- For `http_fetch`: strip HTML first (already done), then truncate
- For `shell`: limit by line count as well as byte count
- Add a `max_result_bytes` field to `nc_chat_request`

**Why this matters:** Free endpoints die silently on oversized
responses. This is the #1 cause of "model not responding" on constrained
setups.

**Files:** `src/agent.c`, `src/nc.h`

---

## P2 — Medium (polish and DX)

### 7. Built-in Memory Backend Options

**Current:** Only flat-file TSV with token-based keyword matching.
(`memory.c`)

**Options to add:**
- **SQLite backend** — simple `CREATE VIRTUAL TABLE ... USING fts5(...)`
  for proper full-text search. Requires `libsqlite3` linked at build.
  - Flag: `#ifdef NC_HAVE_SQLITE3`, fallback to flat-file
- **Embedding backend** — use a tiny local ONNX model
  (e.g., `all-MiniLM-L6-v2` quantized via `ggml`) for semantic search
  - High complexity, low priority — add only if Luckfox deployment demands it
- **Better keyword search** — improved tokenizer that handles
  Indonesian/Java stemming (common in Azzar's context)

**Files:** `src/memory.c` (new backends), `Makefile` (conditional linking)

### 8. MCP Client Resilience

**Current:** MCP servers that die are not restarted. Tool calls to
dead servers fail silently. Tool results only parse `type: text`
content blocks. (`mcp.c`)

**Needed:**
- Health check: periodic `ping` or `tools/list` on idle servers
- Auto-restart: if server dies, respawn it (up to 3 retries)
- Handle `resource` and `image` content types in tool results
- Better timeout handling: different timeout for handshake (10s) vs
  tool execution (configurable, default 60s)
- Log MCP stderr output (currently not captured)

**Files:** `src/mcp.c`

### 9. Cost Tracking & Budget Management

**Current:** `nc_config` has `cost_enabled`, `cost_daily_limit_usd`,
`cost_monthly_limit_usd` but no actual tracking logic.

**Needed:**
- After each LLM call, log `completion_tokens` + `prompt_tokens`
- Estimate cost based on model pricing (local table)
- Store running totals in flat-file, reset daily/monthly
- Block expensive calls (e.g., long context) when budget is low
- Block fallback to paid provider when budget exhausted
- Expose as `/cost` command via Telegram

**Why this matters for free goal:** Need to ensure T1a never accidentally
uses a paid provider. Budget tracking with automatic freeze prevents
surprise bills.

**Files:** `src/agent.c`, `src/provider.c`, `src/commands.c`

### 10. Commands System Expansion

**Current:** Only 4 Telegram commands: `/status`, `/reset`, `/restart`,
`/help`. (`commands.c`)

**Needed:**
- `/memory` — query/manage memory
- `/cost` — show current budget usage
- `/model` — switch model on the fly
- `/providers` — show available providers and their status
- `/uptime` — time since last restart
- `/debug` — toggle verbose logging per-chat

**Files:** `src/commands.c`, `src/nc.h`

---

## P3 — Low (nice to have / stretch)

### 11. Gateway HTTP API

The HTTP gateway (`gateway.c`) exists but is minimal. Could be extended
to serve as a REST API for external tools to interact with T1a:
- WebSocket for streaming responses
- REST endpoint for direct agent chat
- Health check endpoint (for Kuma/UptimeRobot monitoring)
- OpenAPI-like schema

### 12. Discord / Slack Channels

`nc_channel_discord` and `nc_channel_slack` are declared in `nc.h` but
not implemented. A Discord gateway would be useful for the Beranda Wirson
community channels.

### 13. Test Coverage

The codebase has an excellent test framework (`#ifdef NC_TEST`) with arena,
string, JSON, config, memory, and HTTP tests. Running `make test` produces
structured output. Coverage gaps:
- Provider integration tests (mock HTTP)
- MCP handshake simulation
- Channel message parsing
- Rate limiter / action tracking

### 14. Luckfox Pico Mini Deployment Guide

Once the P0 items are complete, T1a should trivially run on Luckfox Pico
Mini (Cortex-A7, 128MB RAM). A deployment guide would cover:
- Cross-compilation for ARM Cortex-A7
- Minimal Linux image (Buildroot or Alpine)
- Systemd unit or init script
- WiFi connection management
- Power monitoring (UPS battery check)
- Headless first-boot configuration

### 15. Self-Metrics / Observability

- Track uptime, total messages, total tool calls, errors by category
- Expose as a JSON status endpoint (or `/status` command)
- Store in memory for trend analysis

---

## Current Architecture Map

```
main.c                               Entry point: onboard/agent/gateway/status/doctor
 ├─ config.c                         JSON config + ENV overrides
 ├─ agent.c                          Chat loop: prompt → LLM → tools → response
 │   ├─ provider.c                   OpenAI / Anthropic / chained providers
 │   ├─ commands.c                   Tool registration + built-in commands
 │   │   ├─ tools.c                  13 built-in tools (shell, file, memory, etc.)
 │   │   ├─ mcp.c                    MCP client → spawns child servers
 │   │   └─ acp_client.c             ACP delegation (incomplete)
 │   └─ memory.c                     Flat-file keyword memory backend
 ├─ channel.c / channel_cli.c        Telegram / CLI poll loops
 ├─ gateway.c                        HTTP REST gateway
 ├─ http.c                           BearSSL native TLS
 ├─ json.c                           JSON parser + writer
 └─ arena.c / util.c                 Arena allocator + string helpers
```

## Priority Matrix

| Item | Effort | Impact | For Free? | For SBC? |
|------|--------|--------|-----------|----------|
| 1. Context summarization | 3 days | High | ✅ critical | ✅ critical |
| 2. Streaming reply flow | 5 days | High | ✅ | ✅ |
| 3. Free provider support | 4 days | High | ✅ required | ✅ |
| 4. ACP completion | 7 days | Medium | ⬜ | ✅ |
| 5. Telegram split | 1 day | Medium | ⬜ | ⬜ |
| 6. Tool result truncation | 1 day | High | ✅ critical | ✅ critical |
| 7. Memory backends | 5 days | Medium | ⬜ | ⬜ |
| 8. MCP resilience | 3 days | Medium | ⬜ | ✅ |
| 9. Cost tracking | 2 days | Medium | ✅ required | ⬜ |
| 10. Expand commands | 2 days | Low | ⬜ | ⬜ |

**Recommended first sprint: Items 1, 2, 3, 6** — these directly
enable running T1a on free endpoints without breaking on long
conversations or large tool outputs.
