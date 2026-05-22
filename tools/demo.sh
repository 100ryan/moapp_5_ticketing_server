#!/usr/bin/env bash
# 제안서 8.2 시나리오용 시연 환경.
# 단일 PC 한계로 10만→1만 규모로 축소. 비율(매크로 80% + 정상 20%)은 그대로.
#
# 사용:
#   ./tools/demo.sh            # 서버 띄우기 (시연용 버킷 파라미터)
#   ./tools/demo.sh test       # 다른 터미널에서 부하 발생기 실행

set -e
cd "$(dirname "$0")/.."

# 시연용 파라미터 (메인서버 처리량 시뮬레이션)
export BUCKET_CAP=200          # 버킷 capacity 200
export BUCKET_RATE=200         # 초당 200명 처리
export QUEUE_TTL=300           # 큐 TTL 5분

if [ "$1" = "test" ]; then
    # 부하 발생: 정상 200 + 매크로 800 = 1000명, 8:2 비율 유지
    NORMAL=${NORMAL:-200}
    MACRO=${MACRO:-800}
    RAMP=${RAMP:-3}
    exec python3 tools/loadtest.py "$NORMAL" "$MACRO" "$RAMP"
fi

make -s

if [ ! -f data/seats.bin ]; then
    ./tools/init_ticket_file
fi

cleanup() { echo; echo "[demo] 종료"; kill ${MAIN_PID:-0} ${DAEMON_PID:-0} 2>/dev/null || true; }
trap cleanup EXIT INT TERM

./main_server/main_server &
MAIN_PID=$!
sleep 0.3

./daemon/daemon &
DAEMON_PID=$!

echo
echo "■ 시연 환경 가동 (BUCKET_CAP=$BUCKET_CAP, BUCKET_RATE=$BUCKET_RATE/s, QUEUE_TTL=${QUEUE_TTL}s)"
echo "■ 부하 발생기 실행: 새 WSL 터미널에서 './tools/demo.sh test'"
echo

wait
