#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if [ -f ".env" ]; then
    export $(grep -v '^#' .env | xargs)
fi

while true; do
    ./noclaw agent --channel telegram >> /tmp/t1a_telegram.log 2>&1
    sleep 5
done
