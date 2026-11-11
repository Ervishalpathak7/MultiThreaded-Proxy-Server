#!/usr/bin/env sh
# Start two demo origin servers on ports 9001 and 9002 (Ctrl-C stops both).
set -e
cd "$(dirname "$0")/.."
python3 scripts/backend.py 9001 backend-A &
A=$!
python3 scripts/backend.py 9002 backend-B &
B=$!
trap 'kill $A $B 2>/dev/null' INT TERM EXIT
wait
