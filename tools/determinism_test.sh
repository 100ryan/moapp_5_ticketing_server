#!/usr/bin/env bash
# 결정적(deterministic) 동작 증명 — 같은 부하 10회 반복.
# 매 회마다 정상 RPS, RESERVE p50/p99 기록. 평균/표준편차/CV 계산.
# CV (변동계수) < 5% 이면 "결정적" 으로 봐도 무방.
set -e
cd "$(dirname "$0")/.."

ITER=10
NORMAL=2000
MACRO=8000
RAMP=5

# 깨끗하게 시작
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

echo "==== 결정적 동작 검증 — 같은 부하 $ITER 회 반복 ===="
echo "    부하: 정상 $NORMAL + 매크로 $MACRO, ramp ${RAMP}s"
echo ""

# CSV 헤더
echo "iter,normal_rps,total_rps,hello_p50,hello_p99,auth_p50,auth_p99,reserve_p50,reserve_p99,total_p50,total_p99" > /tmp/determinism.csv

for i in $(seq 1 $ITER); do
    # seat 초기화 매 회
    rm -f data/seats.bin
    ./tools/init_ticket_file > /dev/null

    LOG=/tmp/det_${i}.txt
    python3 tools/loadtest.py $NORMAL $MACRO $RAMP > $LOG 2>&1

    NORMAL_RPS=$(grep "정상 사용자 완료 RPS" $LOG | awk -F: '{print $2}' | awk '{print $1}')
    TOTAL_RPS=$(grep "전체 요청 RPS" $LOG | awk -F: '{print $2}' | awk '{print $1}')

    HELLO_P50=$(grep "HELLO    :" $LOG | grep -oE 'p50=[ ]*[0-9.]+' | awk -F= '{print $2}' | tr -d ' ')
    HELLO_P99=$(grep "HELLO    :" $LOG | grep -oE 'p99=[ ]*[0-9.]+' | head -1 | awk -F= '{print $2}' | tr -d ' ')
    AUTH_P50=$(grep "AUTH     :" $LOG | grep -oE 'p50=[ ]*[0-9.]+' | awk -F= '{print $2}' | tr -d ' ')
    AUTH_P99=$(grep "AUTH     :" $LOG | grep -oE 'p99=[ ]*[0-9.]+' | head -1 | awk -F= '{print $2}' | tr -d ' ')
    RES_P50=$(grep "RESERVE  :" $LOG | grep -oE 'p50=[ ]*[0-9.]+' | awk -F= '{print $2}' | tr -d ' ')
    RES_P99=$(grep "RESERVE  :" $LOG | grep -oE 'p99=[ ]*[0-9.]+' | head -1 | awk -F= '{print $2}' | tr -d ' ')
    TOT_P50=$(grep "TOTAL    :" $LOG | grep -oE 'p50=[ ]*[0-9.]+' | awk -F= '{print $2}' | tr -d ' ')
    TOT_P99=$(grep "TOTAL    :" $LOG | grep -oE 'p99=[ ]*[0-9.]+' | head -1 | awk -F= '{print $2}' | tr -d ' ')

    echo "$i,$NORMAL_RPS,$TOTAL_RPS,$HELLO_P50,$HELLO_P99,$AUTH_P50,$AUTH_P99,$RES_P50,$RES_P99,$TOT_P50,$TOT_P99" >> /tmp/determinism.csv
    echo "[iter $i] normal_rps=$NORMAL_RPS  RESERVE p50=$RES_P50  p99=$RES_P99  TOTAL p99=$TOT_P99"
done

echo ""
echo "==== 통계 (평균 · 표준편차 · 변동계수 CV) ===="

python3 - <<'PY'
import csv, statistics
with open('/tmp/determinism.csv') as f:
    reader = csv.DictReader(f)
    rows = list(reader)

fields = ['normal_rps', 'total_rps', 'hello_p99', 'auth_p99', 'reserve_p99', 'total_p99', 'reserve_p50']
print(f"{'지표':<14} {'mean':>10} {'stdev':>10} {'CV(%)':>10} {'min':>10} {'max':>10}")
print("-" * 66)
for fld in fields:
    vals = [float(r[fld]) for r in rows if r[fld]]
    if not vals: continue
    m = statistics.mean(vals)
    sd = statistics.stdev(vals) if len(vals) > 1 else 0
    cv = sd / m * 100 if m else 0
    print(f"{fld:<14} {m:>10.3f} {sd:>10.3f} {cv:>10.2f} {min(vals):>10.3f} {max(vals):>10.3f}")

print()
print("→ CV < 5% : 결정적 동작 인정")
print("→ CV 5-10%: 약한 변동, 실용상 결정적")
print("→ CV > 10%: 비결정적")
PY

pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
