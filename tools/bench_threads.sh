#!/usr/bin/env bash
# 멀티스레드 vs 단일스레드 벤치 — queue_server 만 모드 바꾸고 동일 부하로 측정.
# 사용: bash tools/bench_threads.sh <THREADS> [NORMAL] [MACRO] [RAMP]
set -e
cd "$(dirname "$0")/.."

THREADS=$1
NORMAL=${2:-5000}
MACRO=${3:-45000}
RAMP=${4:-15}

# 기존 서버 정리 (SIGKILL — 멀티스레드 종료 시 pthread_cancel 이슈 회피)
pkill -9 -f 'main_server/main_server'   2>/dev/null || true
pkill -9 -f 'queue_server/queue_server' 2>/dev/null || true
pkill -9 -f 'daemon/daemon'              2>/dev/null || true
sleep 0.7

rm -f data/seats.bin
./tools/init_ticket_file > /dev/null

nohup ./main_server/main_server > /tmp/main.log 2>&1 &
sleep 0.3
QUEUE_THREADS=$THREADS nohup ./queue_server/queue_server > /tmp/queue.log 2>&1 &
QPID=$!
sleep 0.3
nohup ./daemon/daemon > /tmp/daemon.log 2>&1 &
sleep 0.6

echo "========== QUEUE_THREADS=$THREADS / 정상 $NORMAL + 매크로 $MACRO / ramp ${RAMP}s =========="

# queue_server CPU 샘플링 (코어 1개 = 100%)
CPU_FILE=/tmp/qcpu.$THREADS.txt
: > $CPU_FILE
(while kill -0 $QPID 2>/dev/null; do
    ps -p $QPID -o pcpu= 2>/dev/null >> $CPU_FILE
    sleep 0.5
done) &
CPUWATCH=$!

python3 tools/loadtest.py $NORMAL $MACRO $RAMP 2>&1 | tail -22

kill $CPUWATCH 2>/dev/null || true

echo
awk 'NF{s+=$1; n++; if($1>m)m=$1} END{if(n) printf "[queue CPU] avg=%.1f%%  max=%.1f%%  (코어 1개=100%%, 16코어 머신)\n", s/n, m; else print "[queue CPU] no samples"}' $CPU_FILE
