# T1a — Pure-C AI Agent for Embedded Devices

T1a is an ultra-lightweight AI agent built in C, designed to run one-per-device
on resource-constrained hardware (Luckfox Pico Mini, ESP32-class SBCs).
~120KB binary, ~10MiB RAM, BearSSL only.

## Key Features

- **OpenCode provider** — Free API (`opencode.ai/zen/v1`), per-device rate limit.
  Models: `deepseek-v4-flash-free` (1M ctx), `mimo-v2.5-free`, `nemotron-3-ultra-free`.
- **3 built-in MCP tools** — Reasoning (sequential thinking), search (Tavily direct
  HTTPS), memory (guardian entity-relation). No Node.js required.
- **Persistent guardian memory** — JSONL-backed entity store with keyword search.
  Shared between memory backend and guardian_memory tool.
- **16+ built-in tools** — shell, file I/O, HTTP fetch, calculator, hash, base64,
  sys info, directory listing, time/timezone, env get.
- **Telegram / CLI channels** — Long-poll for TG, interactive for CLI. Message
  offset persistence across restarts.
- **Caveman system prompt** — <200 token keyword-driven directive. Brevity first.
- **Sliding window context** — 256 messages max, keep 75% on trim. 1M token
  model context handles embedded use cases.

## Architecture

```
src/main.c              Entry: agent / gateway / status / doctor
src/config.c            JSON config + ENV overrides → OpenCode endpoint
src/agent.c             Chat loop: push_msg → LLM → tools → response
src/provider.c          OpenCode provider (OpenAI-compatible)
src/commands.c          Tool registration + Telegram commands
src/tools.c             13 built-in tools
src/mcp_builtin.c       reasoning + tavily_search + guardian_memory
src/mcp.c               External MCP client (optional)
src/memory.c            Guardian memory backend (JSONL persistence)
src/channel.c           Telegram long-poll
src/gateway.c           HTTP REST gateway
src/http.c              BearSSL native TLS (no libcurl, no OpenSSL)
src/json.c              JSON parser + writer
src/arena.c             Arena allocator
```

## Quick Start

```bash
# Build
make clean && make release

# First-time setup
./noclaw onboard \
  --api-key "sk-..." \
  --provider opencode \
  --model deepseek-v4-flash-free

# Run as Telegram daemon
./noclaw agent --channel telegram

# One-shot via CLI
./noclaw agent -m "Apa kabar?"

# Run diagnostics
./noclaw doctor
```

## Systemd Service

```bash
cat > ~/.config/systemd/user/t1a.service <<EOF
[Unit]
Description=T1a AI Agent
After=network.target

[Service]
WorkingDirectory=$(pwd)
ExecStart=$(pwd)/noclaw agent --channel telegram
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now t1a.service
```

## Commands

| Command | Description |
|---------|-------------|
| `/status` | Unit health, tools, memory backend |
| `/reset` | Clear chat history |
| `/restart` | Reboot T1a binary |
| `/help` | Command list |

## Philosophy

- **Zero external deps** — Pure C, BearSSL for TLS. No Python, Node.js, or SQLite.
- **Per-device scaling** — Each T1a appliance uses its own OpenCode free tier.
  No central API proxy or key bottleneck.
- **Efficiency first** — No wasted cycles, no unnecessary allocations.

---
*"Wong edan mah ajaib."*
