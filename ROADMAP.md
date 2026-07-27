# T1a — Roadmap & Findings

> Status: Jul 2026 — OpenCode provider live, binary 112KB
> Binary: ~112KB (stripped) | RAM: ~10MiB | Deps: BearSSL only

## Overview

T1a (noclaw) is a pure-C AI agent designed for resource-constrained devices
(Luckfox Pico Mini, ESP32-class SBCs). It is meant to run **one per device**,
each connecting directly to the OpenCode free API (`opencode.ai/zen/v1`).
This gives per-device rate limiting, no central API key bottleneck, and
true horizontal scaling.

### Architecture Vision: T1a Appliances

```
┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│ Robot Arm    │   │ Smart Robot  │   │ Sensor Hub   │
│ (T1a)        │   │ (T1a)        │   │ (T1a)        │
├──────────────┤   ├──────────────┤   ├──────────────┤
│OpenCode free │   │OpenCode free │   │OpenCode free │
│ per-device   │   │ per-device   │   │ per-device   │
└──────┬───────┘   └──────┬───────┘   └──────┬───────┘
       │                  │                  │
       └──────────────────┼──────────────────┘
                         ▼
              Telegram / MQTT / Web
              (Orchestration Layer)
```

No central LLM proxy. Each T1a is independent, free, and horizontally
scalable to dozens of devices.

---

## ✅ Done (July 2026 Sprint)

### D1. OpenCode Dedicated Provider

Replaced multi-provider (OpenAI + Anthropic + chain) with a single
`nc_provider_opencode()` targeting `https://opencode.ai/zen/v1`.

**Changes:**
- Removed Anthropic provider (~400 lines)
- Removed provider chaining logic
- Added `nc_provider_opencode()` — sets OpenCode URL + model
- Updated config defaults: `deepseek-v4-flash-free` (free, 1M context)
- Reasoning model fix: fall back to `reasoning_content` / `reasoning`
  fields when `content` is null (DeepSeek, MiMo, Nemotron behavior)

**Commits:**
- `9fa3c79` — refactor: replace multi-provider with dedicated OpenCode
- `4507198` — fix: handle reasoning model content in provider parser

### D2. ROADMAP.md Created

Replaced the sparse `TODO.md` with a comprehensive 15-item roadmap
based on a full source audit of all 17 C source files.

**Commit:** `d105bf3` — Add comprehensive ROADMAP with 15-item prioritized plan

---

## OpenCode — Model Reference

| Model | API | Context | Cost | Notes |
|-------|-----|---------|------|-------|
| `deepseek-v4-flash-free` | OpenAI-compat | 1,000,000 | $0 | **Default.** Reasoning chain |
| `mimo-v2.5-free` | OpenAI-compat | 262,144 | $0 | Reasoning chain |
| `nemotron-3-ultra-free` | OpenAI-compat | 128,000 | $0 | Works with content field |
| `north-mini-code-free` | OpenAI-compat | 128,000 | $0 | Coding-focused |

All free models use endpoint: `https://opencode.ai/zen/v1/chat/completions`

---

## P0 — Critical (blocks SBC deployment)

### 1. Context Management — Summarization Before Trim

**Current:** Old messages are dropped silently without summarization.
(`agent.c:agent_push_msg`)

**Approach:** Integrate with **project-guardian** (built-in). Before
trimming, store a summary via guardian context API. This avoids
building a separate summarization loop.

**Files:** `src/agent.c`, new `guardian/` module

### 2. Tool Result Size Limit & Truncation

**Current:** Tool results can exceed LLM context window, causing
silent failures on free endpoints with small context.

**Needed:**
- Truncate tool results >4KB with `[...truncated: N bytes]` note
- For shell/http_fetch/file_read tools
- Add `max_result_bytes` to tool config

**Files:** `src/agent.c`, `src/tools.c`, `src/nc.h`

### 3. Streaming Reply Flow

**Current:** Fully synchronous — user sees nothing during generation.
With free endpoints latency can hit 30-60s.

**Needed:**
- SSE streaming: `"stream": true` in request body
- Parse `data: {...}` chunks incrementally
- Telegram: keep typing indicator alive during generation

**Files:** `src/provider.c`, `src/channel.c`

---

## P1 — High (core capability stack)

### 4. Built-in MCP Servers — Reasoning

**Current:** External MCP servers spawned via `mcp.json`. Tavily and
sequential-thinking depend on Node.js runtime.

**Plan:** Port `sequential-thinking` reasoning tool to pure C,
bundled directly into the T1a binary. No external process needed.

**Why C:** The sequential-thinking MCP server is a simple state machine
with an array of thoughts. Trivially implementable in <200 lines of C.

**Files:** New `src/mcp_reasoning.c`

### 5. Built-in MCP Servers — Search (Tavily + Research Assistant)

**Current:** Tavily MCP requires Node.js and an API key. It also
failed with SSL cert issues on this host.

**Plan:** Bundle a lightweight research/search module that:
- Calls Tavily API directly via BearSSL HTTP (already have `http.c`)
- Integrates with our research-assistant workflow
- No Node.js dependency, no external MCP process
- Returns clean markdown summaries

**Files:** New `src/mcp_search.c`, `src/mcp_tavily.c`

### 6. Built-in Memory & Context — Project Guardian

**Current:** Flat-file TSV with basic keyword matching.

**Plan:** Port **project-guardian** core to a native C module:
- Entity-relation graph stored in flat files (no SQLite needed on SBC)
- Observation CRUD: add, query, delete
- Context summarization for sliding window
- Thread-safe for daemon use

**Files:** New `src/guardian/` directory (entity.c, relation.c, query.c)

### 7. Caveman Protocol — System Prompt

**Current:** Standard verbose system prompt in `agent.c:load_sys_prompt`.

**Plan:** Replace with caveman-style directive:
- Minimal tokens, keyword-driven
- No fluff, no corporate language
- Instructs model to be brief, avoid explanations, use tools silently
- Single paragraph, <200 tokens

**Files:** `src/agent.c`

---

## P2 — Medium (polish and DX)

### 8. MCP Client Resilience

**Current:** MCP servers that die are not restarted. (`mcp.c`)

**Needed:**
- Auto-restart dead servers (up to 3 retries)
- Handle `resource` and `image` content types
- Log MCP stderr output

**Files:** `src/mcp.c`

### 9. Telegram Response Splitting

Telegram hard limit is 4096 chars. Split long responses cleanly
at sentence boundaries without breaking Markdown formatting.

**Files:** `src/channel.c`

### 10. Commands System Expansion

**Current:** 4 Telegram commands: `/status`, `/reset`, `/restart`, `/help`.

**Add:**
- `/memory` — query/manage memories
- `/model` — switch model on the fly
- `/uptime` — time since last restart
- `/caveman` — toggle caveman mode

**Files:** `src/commands.c`

### 11. Cost Tracking (Free Guarantee)

Even though all models are free, add tracking to ensure T1a never
accidentally uses a paid model. Log tokens per call, flag paid models.

**Files:** `src/agent.c`, `src/provider.c`

---

## P3 — Low (nice to have / stretch)

### 12. Luckfox Pico Mini Deployment Guide

Cross-compilation steps, minimal Linux image (Buildroot/Alpine),
systemd unit, WiFi config, power monitoring.

### 13. Gateway HTTP API

Extend `gateway.c` to expose REST endpoints for external monitoring
(Kuma/UptimeRobot). WebSocket for streaming.

### 14. Test Coverage

Expand existing `#ifdef NC_TEST` suite: provider mock, MCP simulation,
channel parsing, rate limiter.

### 15. Self-Metrics

Track uptime, total messages, tool calls, errors. Expose via `/status`.

---

## Current Architecture Map

```
main.c                               Entry point
 ├─ config.c                         JSON config + ENV overrides → OpenCode
 ├─ agent.c                          Chat loop → LLM → tools → response
 │   ├─ provider.c                   OpenCode provider only [SIMPLIFIED]
 │   ├─ commands.c                   Tool registration + Telegram commands
 │   │   ├─ tools.c                  13 built-in tools
 │   │   ├─ mcp.c                    MCP client (external servers)
 │   │   ├─ mcp_reasoning.c         [PLANNED] Built-in reasoning
 │   │   ├─ mcp_search.c            [PLANNED] Built-in search
 │   │   └─ guardian/               [PLANNED] Context & memory engine
 │   └─ memory.c                     Flat-file keyword memory
 ├─ channel.c / channel_cli.c        Telegram / CLI channels
 ├─ gateway.c                        HTTP gateway
 ├─ http.c                           BearSSL native TLS
 ├─ json.c                           JSON parser + writer
 └─ arena.c / util.c                 Arena allocator + helpers
```

## Priority Matrix (Revised)

| # | Item | Sprint | Effort | Impact | Notes |
|---|------|--------|--------|--------|-------|
| D1 | OpenCode provider | ✅ Done | 1d | Critical | Live |
| D2 | ROADMAP.md | ✅ Done | — | — | Live |
| 1 | Context mgmt + guardian | 2 | 5d | Critical | Memory for SBC |
| 2 | Tool result truncation | 2 | 1d | Critical | Prevents silent failures |
| 3 | Streaming reply flow | 2 | 3d | High | UX on slow models |
| 4 | Built-in reasoning MCP | 3 | 2d | High | No Node dep |
| 5 | Built-in search MCP | 3 | 3d | High | No Node dep |
| 6 | Guardian memory native | 3 | 5d | High | Full context engine |
| 7 | Caveman system prompt | 2 | 0.5d | Medium | Token savings |
| 8 | MCP resilience | 3 | 2d | Medium | |
| 9 | Telegram split | 2 | 1d | Medium | |
| 10 | Commands expansion | 4 | 1d | Low | |
| 11 | Cost tracking | 4 | 1d | Low | |
| 12 | Luckfox guide | 5 | 2d | Low | |
| 13-15 | Stretch | 5+ | varies | Low | |

**Sprint 2 focus:** Context + guardian (1), tool truncation (2),
streaming (3), caveman prompt (7), telegram split (9).
