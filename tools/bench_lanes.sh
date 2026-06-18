#!/usr/bin/env bash
# 듀얼 레인 큐 효과 측정 — 싱글(1 lane) vs 듀얼(2 lane) vs 쿼드(4 lane)
set -e
cd "$(dirname "$0")/.."

NORMAL=5000
MACRO=15000
RAMP=10

run_lanes() {
    local n=$1
    pkill -9 -f main_server/main_server   2>/dev/null || true
    pkill -9 -f queue_server/queue_server 2>/dev/null || true
    pkill -9 -f daemon/daemon              2>/dev/null || true
    sleep 0.7
    rm -f data/seats.bin
    ./tools/init_ticket_file > /dev/null

    nohup ./main_server/main_server > /tmp/main.log 2>&1 &
    sleep 0.3

    # N 개 queue_server (포트 9092, 9093, ...)
    PORTS=""
    for i in $(seq 0 $((n - 1))); do
        port=$((9092 + i))
        QUEUE_PORT=$port nohup ./queue_server/queue_server > /tmp/queue_$i.log 2>&1 &
        if [ -z "$PORTS" ]; then PORTS=$port; else PORTS="$PORTS,$port"; fi
        sleep 0.15
    done
    sleep 0.3

    LANE_PORTS="$PORTS" nohup ./daemon/daemon > /tmp/daemon.log 2>&1 &
    sleep 0.7

    echo "==== LANES=$n  (queue ports: $PORTS) ===="
    python3 tools/loadtest.py $NORMAL $MACRO $RAMP 2>&1 | grep -E "(완료|RPS|AUTH     :|RESERVE  :|TOTAL    :|매크로 차단율)"
    echo ""
}

run_lanes 1
run_lanes 2
run_lanes 4

pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
