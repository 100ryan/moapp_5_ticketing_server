#!/usr/bin/env bash
# 세 프로세스 같이 띄우기 (예매 서버 → 대기열 서버 → 데몬)
# - 시작 전 잔존 프로세스 정리 + 포트 비기 대기 (TIME_WAIT)
# - 각 단계마다 포트 실제로 잡혔는지 확인하고 실패면 명확히 출력
# - Ctrl+C 로 셋 다 정리
set -e
cd "$(dirname "$0")"

make -s

if [ ! -f data/seats.bin ]; then
    ./tools/init_ticket_file
fi

# 잔존 프로세스 정리 — 이전 인스턴스가 9092 등을 잡고 있으면 새 큐가 bind 실패
echo "[run] 잔존 프로세스 정리..."
pkill -9 -x main_server  2>/dev/null || true
pkill -9 -x queue_server 2>/dev/null || true
pkill -9 -x daemon       2>/dev/null || true

# 포트 비워질 때까지 폴링 (TIME_WAIT 풀림). 최대 90초.
wait_port_free() {
    local port=$1
    for _ in $(seq 1 90); do
        if ! ss -tln 2>/dev/null | grep -qE ":${port}\b"; then return 0; fi
        sleep 1
    done
    return 1
}

# 포트 잡혔는지 폴링. 최대 4초.
wait_port_listening() {
    local port=$1
    for _ in $(seq 1 40); do
        if ss -tln 2>/dev/null | grep -qE ":${port}\b"; then return 0; fi
        sleep 0.1
    done
    return 1
}

echo -n "[run] 포트 9090/9091/9092 비기 대기..."
wait_port_free 9090 && wait_port_free 9091 && wait_port_free 9092
echo " OK"

cleanup() {
    echo
    echo "[run] 종료"
    kill ${DAEMON_PID:-0} ${QUEUE_PID:-0} ${MAIN_PID:-0} 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ---- 메인 서버 (9091) ------------------------------------------------------
./main_server/main_server &
MAIN_PID=$!
if ! wait_port_listening 9091; then
    echo "[run] ❌ 메인 서버 9091 bind 실패 — 이미 누가 잡고 있나? (ss -tlnp)"
    exit 1
fi
echo "[run] ✅ 메인 서버 (PID $MAIN_PID, 9091)"

# ---- 대기열 서버 (9092) -----------------------------------------------------
./queue_server/queue_server &
QUEUE_PID=$!
if ! wait_port_listening 9092; then
    echo "[run] ❌ 대기열 서버 9092 bind 실패 — 이미 누가 잡고 있나? (ss -tlnp)"
    exit 1
fi
echo "[run] ✅ 대기열 서버 (PID $QUEUE_PID, 9092)"

# ---- 데몬 (9090) -----------------------------------------------------------
./daemon/daemon &
DAEMON_PID=$!
if ! wait_port_listening 9090; then
    echo "[run] ❌ 데몬 9090 bind 실패 — 이미 누가 잡고 있나? (ss -tlnp)"
    exit 1
fi
echo "[run] ✅ 데몬 (PID $DAEMON_PID, 9090)"

echo
echo "[run] 🎫 모든 서버 가동 — Ctrl+C 로 종료"
wait
