# T1a Agent Client Protocol (ACP) - Master Orchestrator Plan

## 1. The Architecture: T1a as the Client (Controller)
Instead of `t1a` acting as an ACP server for an IDE, `t1a` will act as the **ACP Client**. It will spawn other AI agents (like `gemini-cli` or `codex`) as child processes and delegate complex tasks to them using the ACP JSON-RPC standard over `stdio`.

This turns `t1a` into a "Master Orchestrator," keeping the core agent ultra-lightweight (3MB) while granting it the massive capabilities of commercial CLI agents on demand.

## 2. Core Components Needed
1. **`acp_client.c` (The Subprocess Manager):**
   - Leverages the existing subprocess logic from `mcp.c` (`mcp_proc_start`).
   - Spawns the sub-agent (e.g., `gemini --acp`).
   - Connects to its `stdin`/`stdout` pipes.

2. **The ACP Handshake:**
   - `t1a` sends the `initialize` request to the sub-agent, defining what capabilities it allows.
   - Waits for the `initialize` response.
   - Sends `notifications/initialized`.

3. **Session Delegation & Streaming:**
   - When the user asks `t1a` to delegate a task, `t1a` sends `session/new` (or `session/prompt`) to the sub-agent.
   - `t1a` enters a non-blocking `select()` loop to read `session/update` notifications from the sub-agent.
   - `t1a` streams the `params.content` from these updates directly to the active `nc_channel` (e.g., Telegram or CLI), so the user sees the sub-agent "typing" in real-time.

4. **Permission Management (Crucial):**
   - Commercial agents will try to use tools (like reading files or running shell commands). They will send `session/request_permission`.
   - `t1a` must intercept these, auto-approve safe ones (like reading in the workspace), or deny dangerous ones, acting as a security sandbox.

## 3. Synergy with MCP
`t1a` already supports MCP tools. In the future, `t1a` could theoretically expose its own native C tools (like the PTY shell) to the ACP sub-agents, making `t1a` both an ACP Controller and an MCP Server simultaneously!

## 4. Next Steps
1. Create `acp.h` to define the ACP JSON-RPC structures.
2. Clone the `mcp_server` struct pattern into an `acp_agent` struct.
3. Build the `initialize` handshake sequence.