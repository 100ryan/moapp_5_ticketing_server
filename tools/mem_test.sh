#!/usr/bin/env bash
# 60초 부하 동안 RSS 시계열 측정 — 누수 검증용
set -e
cd "$(dirname "$0")/.."

# 서버 기동
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
echo "queue=$QPID main=$MPID daemon=$DPID"

# RSS 시계열 (1초 간격)
: > /tmp/mem.csv
echo "sec,queue_rss_kb,main_rss_kb,daemon_rss_kb" > /tmp/mem.csv

# RSS 시작값
Q0=$(ps -p $QPID -o rss=)
M0=$(ps -p $MPID -o rss=)
D0=$(ps -p $DPID -o rss=)
echo "[start] queue=${Q0}KB main=${M0}KB daemon=${D0}KB"

# 백그라운드 측정 루프
(
  for i in $(seq 0 60); do
    Q=$(ps -p $QPID -o rss= 2>/dev/null || echo 0)
    M=$(ps -p $MPID -o rss= 2>/dev/null || echo 0)
    D=$(ps -p $DPID -o rss= 2>/dev/null || echo 0)
    echo "$i,$Q,$M,$D" >> /tmp/mem.csv
    sleep 1
  done
) &
MONITOR_PID=$!

# 부하: 5000명 정상 + 15000명 매크로 (총 20K)를 50초간 ramp-up
python3 tools/loadtest.py 5000 15000 45 > /tmp/load.txt 2>&1 &
LOAD_PID=$!

wait $MONITOR_PID
wait $LOAD_PID 2>/dev/null || true

# 최종 RSS
Q1=$(ps -p $QPID -o rss= 2>/dev/null || echo 0)
M1=$(ps -p $MPID -o rss= 2>/dev/null || echo 0)
D1=$(ps -p $DPID -o rss= 2>/dev/null || echo 0)
echo "[end]   queue=${Q1}KB main=${M1}KB daemon=${D1}KB"

# 차이
echo ""
echo "=== RSS 증가량 ==="
echo "queue:  $((Q1 - Q0)) KB"
echo "main:   $((M1 - M0)) KB"
echo "daemon: $((D1 - D0)) KB"

# 최대값
echo ""
echo "=== 최대 RSS ==="
echo "queue:  $(awk -F, 'NR>1 && $2>m{m=$2} END{print m}' /tmp/mem.csv) KB"
echo "main:   $(awk -F, 'NR>1 && $3>m{m=$3} END{print m}' /tmp/mem.csv) KB"
echo "daemon: $(awk -F, 'NR>1 && $4>m{m=$4} END{print m}' /tmp/mem.csv) KB"
