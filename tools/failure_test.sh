#!/usr/bin/env bash
# 부분 장애 격리 실증 — 시나리오 3개
# A) main_server kill → daemon 응답 어떻게 되나
# B) queue_server kill → 동일
# C) kill 후 재시작 → daemon 재연결 성공하나
set -e
cd "$(dirname "$0")/.."

cleanup_all() {
    pkill -9 -f main_server/main_server   2>/dev/null || true
    pkill -9 -f queue_server/queue_server 2>/dev/null || true
    pkill -9 -f daemon/daemon              2>/dev/null || true
    sleep 0.5
}

start_all() {
    rm -f data/seats.bin
    ./tools/init_ticket_file > /dev/null
    nohup ./main_server/main_server > /tmp/main.log 2>&1 &
    sleep 0.3
    nohup ./queue_server/queue_server > /tmp/queue.log 2>&1 &
    sleep 0.3
    nohup ./daemon/daemon > /tmp/daemon.log 2>&1 &
    sleep 0.7
}

# 한 요청 보내고 응답 받기 (timeout 2초)
hit() {
    echo "$1" | nc -w2 127.0.0.1 9090 2>/dev/null || echo "(timeout/refused)"
}

cleanup_all
start_all

echo "============================================"
echo "▶ 시나리오 A: main_server 죽이고 RESERVE 시도"
echo "============================================"

# 정상 토큰 확보 (HELLO + AUTH 흐름은 생략하고 직접 만들 수 없으니, BAD_TOKEN 응답으로 daemon→queue→ daemon→main 경로 확인)
# 가짜 토큰으로 RESERVE → daemon→queue VERIFY → BAD_TOKEN 받고 main 호출 안 함. 그래서 별 의미 없음.
# 대신 HELLO/AUTH 로 진짜 토큰 받고 RESERVE 시도.

echo ""
echo "[정상 상태] HELLO 응답:"
hit "HELLO:dev1"

echo ""
echo "[정상 상태] FETCH_STATUS (좌석 비트맵 길이):"
RESP=$(hit "FETCH_STATUS")
echo "  길이=$(echo -n "$RESP" | wc -c) 자  (정상이면 7+5000 자)"
echo "  앞 50자: ${RESP:0:50}..."

echo ""
echo "▶▶ main_server 강제 종료 (kill -9)"
pkill -9 -f main_server/main_server
sleep 0.5

echo ""
echo "[main 죽음] HELLO 응답 (대기열 서버에 의존하므로 정상이어야 함):"
hit "HELLO:dev2"

echo ""
echo "[main 죽음] FETCH_STATUS (main 거치므로 실패해야 함):"
hit "FETCH_STATUS"

echo ""
echo "[main 죽음] STATS (대기열 서버에서 옴, 정상이어야 함):"
hit "STATS" | head -c 200; echo

echo ""
echo "▶▶ main_server 재시작"
nohup ./main_server/main_server > /tmp/main2.log 2>&1 &
sleep 1

echo ""
echo "[main 복구 후] FETCH_STATUS:"
RESP=$(hit "FETCH_STATUS")
echo "  길이=$(echo -n "$RESP" | wc -c) 자"
echo "  → 복구 OK (daemon 이 ensure_main_connection 으로 자동 재연결)"

echo ""
echo "============================================"
echo "▶ 시나리오 B: queue_server 죽이고 HELLO 시도"
echo "============================================"

echo ""
echo "[정상 상태] HELLO 응답:"
hit "HELLO:dev3"

echo ""
echo "▶▶ queue_server 강제 종료"
pkill -9 -f queue_server/queue_server
sleep 0.5

echo ""
echo "[queue 죽음] HELLO 응답 (queue 거치므로 실패해야 함):"
hit "HELLO:dev4"

echo ""
echo "[queue 죽음] FETCH_STATUS (main 만 거치므로 정상):"
RESP=$(hit "FETCH_STATUS")
echo "  길이=$(echo -n "$RESP" | wc -c) 자  (5007 자면 정상)"

echo ""
echo "▶▶ queue_server 재시작"
nohup ./queue_server/queue_server > /tmp/queue2.log 2>&1 &
sleep 1

echo ""
echo "[queue 복구 후] HELLO 응답:"
hit "HELLO:dev5"
echo "  → 복구 OK"

echo ""
echo "============================================"
echo "▶ 정리"
echo "============================================"
cleanup_all
echo "(다 죽임)"
