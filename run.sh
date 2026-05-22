#!/usr/bin/env bash
# 서버 두 개 같이 띄우기. 종료(Ctrl+C)하면 양쪽 모두 정리.
set -e
cd "$(dirname "$0")"

make -s

if [ ! -f data/seats.bin ]; then
    ./tools/init_ticket_file
fi

cleanup() { echo; echo "[run] 종료"; kill ${MAIN_PID:-0} ${DAEMON_PID:-0} 2>/dev/null || true; }
trap cleanup EXIT INT TERM

./main_server/main_server &
MAIN_PID=$!
sleep 0.3

./daemon/daemon &
DAEMON_PID=$!

wait
