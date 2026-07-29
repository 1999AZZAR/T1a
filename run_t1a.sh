#!/bin/bash
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

[ -f "$HOME/.noclaw/env" ] && source "$HOME/.noclaw/env"

while true; do
    ./noclaw agent --channel telegram >> /tmp/t1a.log 2>&1
    sleep 5
done
