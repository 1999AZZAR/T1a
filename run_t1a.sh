#!/bin/bash
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if [ -f ".env" ]; then
    export $(grep -v '^#' .env | xargs)
fi

[ -f "$HOME/.t1a/env" ] && source "$HOME/.t1a/env"

while true; do
    ./t1a agent --channel telegram >> /tmp/t1a.log 2>&1
    sleep 5
done
