#!/usr/bin/env bash
# 큐 폭주 → 토해내기 시나리오
# - 토큰 발급 속도를 일부러 낮춤 (rate=10/s) → 큐가 빠르게 쌓임
# - 정상 2000명 즉시 폭주 → 거의 모두 큐로 들어감
# - 발급 속도가 따라잡아주면서 일제히 토큰 받음
# - 동시에 RESERVE 폭주 → CAS 정합성 검증 (좌석 일관성 유지하는지)
set -e
cd "$(dirname "$0")/.."

pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
sleep 0.7

rm -f data/seats.bin
./tools/init_ticket_file > /dev/null

nohup ./main_server/main_server > /tmp/main.log 2>&1 &
sleep 0.3
# 토큰 발급 속도 극저 — 의도적 큐 폭증 시뮬레이션
BUCKET_RATE=10 BUCKET_CAP=20 nohup ./queue_server/queue_server > /tmp/queue.log 2>&1 &
sleep 0.3
nohup ./daemon/daemon > /tmp/daemon.log 2>&1 &
sleep 0.7

echo "==== 큐 폭주 시나리오 — RATE=10/s (의도적 저속) ===="
echo "    정상 사용자 2000 명 즉시 폭주 (ramp=0) → 거의 다 큐에 적재"
echo "    매크로 100명만 끼워서 정상 동작 확인"
echo ""

# RESERVE 단계의 CAS 정합성 핵심 점검 = 좌석 두번 안 팔리는가
# loadtest.py 가 정상 사용자 RESERVE 시도하므로, success+fail_taken 합이 좌석 수 한계여야 함.

python3 tools/loadtest.py 2000 100 0 2>&1 | tee /tmp/burst.txt | tail -25

echo ""
echo "==== 좌석 일관성 검증 ===="
# bin 파일에서 1로 마킹된 좌석 수 세기
SOLD=$(python3 -c "
with open('data/seats.bin', 'rb') as f:
    data = f.read()
import struct
seats = struct.unpack('<5000i', data)
print(sum(1 for s in seats if s == 1))
")
# 초기에 7의 배수 (715개) + 새로 팔린 만큼
INITIAL=715
SUCCESS=$(grep '예매 성공' /tmp/burst.txt | grep -oE '[0-9]+$' | tail -1)
echo "  초기 마킹 (7의 배수)       : $INITIAL"
echo "  부하 후 데이터파일 sold    : $SOLD"
echo "  loadtest 보고 success      : $SUCCESS"
echo "  검증: SOLD - INITIAL == SUCCESS ?"
EXPECTED=$((SOLD - INITIAL))
if [ "$EXPECTED" = "$SUCCESS" ]; then
    echo "  ✅ 일치 — CAS 정합성 OK (중복 판매 0)"
else
    echo "  ⚠️ 불일치 — sold-initial=$EXPECTED success=$SUCCESS"
fi

echo ""
echo "==== 대기열 서버 stats ===="
echo "STATS" | nc -w2 127.0.0.1 9090

pkill -9 -f main_server/main_server   2>/dev/null || true
pkill -9 -f queue_server/queue_server 2>/dev/null || true
pkill -9 -f daemon/daemon              2>/dev/null || true
