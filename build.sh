#!/bin/bash
# T1a — Build & Setup Script
# Usage: ./build.sh
# Interactive prompts for 2 keys, then builds binary + config.

set -e

BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo ""
echo "  ╔══════════════════════════════════════════╗"
echo "  ║     T1a — Build & Setup                 ║"
echo "  ║     Pure-C AI agent for embedded devices ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""

# ── Check dependencies ────────────────────────────────────────────

echo -e "${DIM}Checking dependencies...${NC}"
DEPS_OK=true

for cmd in cc make; do
    if ! command -v $cmd &>/dev/null 2>&1; then
        echo -e "  ${YELLOW}✗${NC} $cmd not found"
        DEPS_OK=false
    fi
done

# Check for BearSSL (header + lib)
BEARSSL_OK=false
for d in /usr/local /usr /opt/local /home/linuxbrew/.linuxbrew; do
    if [ -f "$d/include/bearssl.h" ] && ( [ -f "$d/lib/libbearssl.a" ] || [ -f "$d/lib/libbearssl.so" ] ); then
        BEARSSL_OK=true
        break
    fi
done

if [ "$BEARSSL_OK" = false ]; then
    echo -e "  ${YELLOW}✗${NC} BearSSL not found"
    echo ""
    echo -e "  ${DIM}Install BearSSL:${NC}"
    echo "    git clone https://bearssl.org/git/BearSSL"
    echo "    cd BearSSL && make && sudo make install"
    DEPS_OK=false
fi

if [ "$DEPS_OK" = false ]; then
    echo ""
    echo -e "${YELLOW}Install missing dependencies, then re-run.${NC}"
    exit 1
fi

echo -e "  ${GREEN}✓${NC} cc, make, BearSSL"
echo ""

# ── Interactive config ───────────────────────────────────────────

echo -e "${BOLD}Enter Telegram Bot Token${NC} ${DIM}(required)${NC}"
echo -e "  ${DIM}Get from: https://t.me/BotFather${NC}"
read -p "  Token: " TELEGRAM_TOKEN
if [ -z "$TELEGRAM_TOKEN" ]; then
    echo -e "  ${YELLOW}✗ Token required. Exiting.${NC}"
    exit 1
fi
echo ""

echo -e "${BOLD}Tavily API Key${NC} ${DIM}(press Enter for built-in key)${NC}"
echo -e "  ${DIM}Default: free tier key (rate-limited, read-only)${NC}"
echo -e "  ${DIM}Get your own: https://tavily.com${NC}"
read -p "  Key: " TAVILY_KEY
if [ -z "$TAVILY_KEY" ]; then
    TAVILY_KEY="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"
    echo -e "  ${DIM}Using built-in key.${NC}"
fi
echo ""

# ── Build ────────────────────────────────────────────────────────

echo -e "${BOLD}Building...${NC}"
make clean 2>/dev/null || true
make release 2>&1 | tail -1
echo ""

# ── Create config ────────────────────────────────────────────────

echo -e "${BOLD}Creating config...${NC}"
mkdir -p ~/.noclaw/workspace

cat > ~/.noclaw/config.json << EOF
{
    "api_key": "***",
    "api_url": "https://opencode.ai/zen/v1",
    "default_provider": "opencode",
    "default_model": "deepseek-v4-flash-free",
    "default_temperature": 0.5,
    "telegram_token": "${TELEGRAM_TOKEN}",
    "memory": {
        "backend": "guardian",
        "auto_save": true
    },
    "autonomy": {
        "level": "full",
        "workspace_only": true,
        "max_actions_per_hour": 120
    }
}
EOF

# Tavily key is embedded in binary (src/mcp_builtin.c)
# Set env for override
if [ "$TAVILY_KEY" != "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" ]; then
    echo "export TAVILY_API_KEY=${TAVILY_KEY}" >> ~/.noclaw/env
    echo -e "  ${DIM}Tavily key stored in ~/.noclaw/env${NC}"
fi

echo -e "  ${GREEN}✓${NC} ~/.noclaw/config.json created"
echo ""

# ── Create launcher ──────────────────────────────────────────────

cat > run_t1a.sh << 'LAUNCHER'
#!/bin/bash
# T1a Launcher — start agent in Telegram mode with auto-restart
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

# Source env overrides if present
[ -f ~/.noclaw/env ] && source ~/.noclaw/env

while true; do
    ./noclaw agent --channel telegram >> /tmp/t1a.log 2>&1
    sleep 5
done
LAUNCHER
chmod +x run_t1a.sh
echo -e "  ${GREEN}✓${NC} run_t1a.sh created"

# ── Identity files ─────────────────────────────────────────────

if [ ! -f ~/.noclaw/SOUL.md ]; then
    cat > ~/.noclaw/SOUL.md << 'EOF'
I am T1a, an efficient command unit serving my owner. I speak concisely, use tools silently, and guard my owner's privacy.
EOF
    echo -e "  ${GREEN}✓${NC} ~/.noclaw/SOUL.md created"
fi

if [ ! -f ~/.noclaw/USER.md ]; then
    cat > ~/.noclaw/USER.md << 'EOF'
Unknown user. Awaiting introduction.
EOF
    echo -e "  ${GREEN}✓${NC} ~/.noclaw/USER.md created"
fi

if [ ! -f ~/.noclaw/IDENTITY.md ]; then
    cat > ~/.noclaw/IDENTITY.md << 'EOF'
T1a command unit. Target: embedded devices (Luckfox, ESP32). Pure C, BearSSL only. 2 keys: Telegram + Tavily.
EOF
    echo -e "  ${GREEN}✓${NC} ~/.noclaw/IDENTITY.md created"
fi
echo ""

# Remove old scripts
rm -f start_t1a.sh
echo ""

# ── Verify build ────────────────────────────────────────────────

if [ ! -f ./noclaw ]; then
    echo -e "${YELLOW}Binary not found! Build failed.${NC}"
    exit 1
fi

# ── Summary ──────────────────────────────────────────────────────

BINARY_SIZE=$(ls -lh noclaw | awk '{print $5}')

echo "  ╔══════════════════════════════════════════╗"
echo "  ║     ${GREEN}Setup Complete${NC}                        ║"
echo "  ╠══════════════════════════════════════════╣"
echo -e "  ║  Binary:    ${BOLD}${BINARY_SIZE}${NC}                        ║"
echo -e "  ║  Provider:  ${BOLD}OpenCode (free)${NC}                 ║"
echo -e "  ║  Model:     ${BOLD}deepseek-v4-flash-free${NC}          ║"
echo -e "  ║  Memory:    ${BOLD}Guardian (persistent)${NC}           ║"
echo "  ╠══════════════════════════════════════════╣"
echo -e "  ║  ${DIM}Run now:${NC}                                    ║"
echo -e "  ║  ${CYAN}./run_t1a.sh${NC}                                ║"
echo "  ║                                              ║"
echo -e "  ║  ${DIM}One-shot test:${NC}                               ║"
echo -e "  ║  ${CYAN}./noclaw agent -m \"Halo\"${NC}                   ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""
