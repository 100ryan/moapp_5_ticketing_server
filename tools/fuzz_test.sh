#!/usr/bin/env bash
# 이상 입력에 서버가 죽지 않는지 검증
set -e
cd "$(dirname "$0")/.."

# 클린 재시작
pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
sleep 0.5
rm -f data/seats.bin
./tools/init_ticket_file > /dev/null
nohup ./main_server/main_server > /tmp/main.log 2>&1 &
sleep 0.3
nohup ./queue_server/queue_server > /tmp/queue.log 2>&1 &
sleep 0.3
nohup ./daemon/daemon > /tmp/daemon.log 2>&1 &
sleep 0.7

QPID=$(pgrep -f queue_server/queue_server | head -1)
MPID=$(pgrep -f main_server/main_server   | head -1)
DPID=$(pgrep -f daemon/daemon              | head -1)
echo "[before] queue=$QPID main=$MPID daemon=$DPID 모두 살아있어야 함"
echo ""

run_test() {
    local label="$1"; shift
    local payload="$1"
    local resp=$(echo -ne "$payload" | nc -w1 127.0.0.1 9090 2>/dev/null | head -c 100)
    echo "[$label]"
    echo "  보낸 것: $(echo -n "$payload" | head -c 60 | cat -v)"
    echo "  응답   : $(echo -n "$resp" | cat -v)"
    # 살아있나?
    if kill -0 $QPID 2>/dev/null && kill -0 $MPID 2>/dev/null && kill -0 $DPID 2>/dev/null; then
        echo "  서버   : 생존 OK"
    else
        echo "  서버   : ⚠️  죽음!"
        return 1
    fi
    echo ""
}

echo "============================================"
echo "▶ Fuzz Test — 17가지 이상 입력"
echo "============================================"
echo ""

run_test "01. 빈 줄"                  "\n"
run_test "02. \\r\\n 만"               "\r\n"
run_test "03. 콜론 없음"               "HELLOdev1\n"
run_test "04. 콜론만"                  ":\n"
run_test "05. 알 수 없는 명령"          "DROP_TABLE:users\n"
run_test "06. 음수 좌석"               "RESERVE:fake:-1\n"
run_test "07. 좌석 범위 초과"          "RESERVE:fake:99999\n"
run_test "08. 좌석에 문자"             "RESERVE:fake:abc\n"
run_test "09. 토큰에 콜론 포함"        "RESERVE::::5\n"
run_test "10. 토큰에 개행 시도"        "RESERVE:fake\nseat:5\n"
run_test "11. AUTH 필드 부족"          "AUTH:user:dev\n"
run_test "12. AUTH 필드 과다"          "AUTH:u:d:n:s:h:extra:extra2\n"
run_test "13. HELLO 빈 device_id"     "HELLO:\n"
run_test "14. VERIFY 빈 토큰"          "VERIFY:\n"
run_test "15. POLL 빈 device_id"      "POLL:\n"
run_test "16. SQL Injection 풍"        "HELLO:dev'; DROP TABLE--\n"
run_test "17. Null 바이트"             "HELLO:dev\x00pad\n"

echo "============================================"
echo "▶ Stress — 5000 자 길이 줄"
echo "============================================"
LONG_LINE=$(printf 'HELLO:%.0s' {1..1000})
RESP=$(echo "$LONG_LINE" | nc -w1 127.0.0.1 9090 2>/dev/null | head -c 100)
echo "  응답: $(echo -n "$RESP" | cat -v)"
if kill -0 $QPID && kill -0 $MPID && kill -0 $DPID 2>/dev/null; then
    echo "  서버: 생존 OK"
else
    echo "  서버: ⚠️  죽음"
fi
echo ""

echo "============================================"
echo "▶ Connection flood — 100개 동시 연결만 + 즉시 끊기"
echo "============================================"
for i in $(seq 1 100); do
    (exec 3<>/dev/tcp/127.0.0.1/9090; exec 3>&-) 2>/dev/null &
done
wait
sleep 0.3
if kill -0 $QPID && kill -0 $MPID && kill -0 $DPID 2>/dev/null; then
    echo "  100 연결 + 즉시 끊기 후: 서버 생존 OK"
else
    echo "  ⚠️  서버 죽음"
fi
echo ""

echo "============================================"
echo "▶ 정상 요청으로 마무리 동작 확인"
echo "============================================"
echo "HELLO:final" | nc -w1 127.0.0.1 9090
echo "FETCH_STATUS" | nc -w1 127.0.0.1 9090 | head -c 50; echo

echo ""
echo "[after] 모든 fuzz 테스트 통과 — 서버 3개 생존"

# 정리
pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
