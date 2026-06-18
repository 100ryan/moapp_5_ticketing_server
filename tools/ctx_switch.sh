#!/usr/bin/env bash
# 컨텍스트 스위치 측정 — /proc/[pid]/status 직접 (의존성 없음)
# voluntary_ctxt_switches / nonvoluntary_ctxt_switches 의 전후 차이를 측정 시간으로 나눔
set -e
cd "$(dirname "$0")/.."

# 프로세스 + 모든 스레드의 cs 합계
sum_ctxt() {
    local pid=$1
    local vol=0 nvol=0
    for tid in /proc/$pid/task/*/status; do
        [ -r "$tid" ] || continue
        v=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "$tid" 2>/dev/null || echo 0)
        n=$(awk '/^nonvoluntary_ctxt_switches:/ {print $2}' "$tid" 2>/dev/null || echo 0)
        vol=$((vol + ${v:-0}))
        nvol=$((nvol + ${n:-0}))
    done
    echo "$vol $nvol"
}

run_one() {
    local threads=$1
    pkill -9 -f main_server/main_server   2>/dev/null || true
    pkill -9 -f queue_server/queue_server 2>/dev/null || true
    pkill -9 -f daemon/daemon              2>/dev/null || true
    sleep 0.5
    rm -f data/seats.bin
    ./tools/init_ticket_file > /dev/null
    nohup ./main_server/main_server > /tmp/main.log 2>&1 &
    sleep 0.3
    QUEUE_THREADS=$threads nohup ./queue_server/queue_server > /tmp/queue.log 2>&1 &
    sleep 0.3
    nohup ./daemon/daemon > /tmp/daemon.log 2>&1 &
    sleep 0.7

    QPID=$(pgrep -f queue_server/queue_server | head -1)

    echo "==== QUEUE_THREADS=$threads (queue_server PID=$QPID) ===="

    # 부하 시작 전 카운트
    read V0 N0 < <(sum_ctxt $QPID)
    T0=$(date +%s.%N)

    # 부하 — 3000 정상 + 9000 매크로, ramp 5s
    python3 tools/loadtest.py 3000 9000 5 > /tmp/load_${threads}.txt 2>&1
    LOAD_EXIT=$?

    T1=$(date +%s.%N)
    read V1 N1 < <(sum_ctxt $QPID)

    DURATION=$(awk -v a=$T0 -v b=$T1 'BEGIN{printf "%.3f", b-a}')
    DV=$((V1 - V0))
    DN=$((N1 - N0))

    echo "[부하 결과]"
    grep -E "(완료|RPS|RESERVE  :|TOTAL    :)" /tmp/load_${threads}.txt | head -6

    echo ""
    echo "[컨텍스트 스위치 — 부하 기간 ${DURATION}초]"
    echo "  voluntary    cs 총 : $DV"
    echo "  nonvoluntary cs 총 : $DN"
    echo "  voluntary    /sec  : $(awk -v c=$DV -v d=$DURATION 'BEGIN{printf "%.1f", c/d}')"
    echo "  nonvoluntary /sec  : $(awk -v c=$DN -v d=$DURATION 'BEGIN{printf "%.1f", c/d}')"
    echo "  합계 cs/sec        : $(awk -v c=$((DV+DN)) -v d=$DURATION 'BEGIN{printf "%.1f", c/d}')"
    echo ""
}

run_one 1
run_one 8

pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
echo "[정리 완료]"
