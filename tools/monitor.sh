#!/usr/bin/env bash
# 데몬에 1초마다 STATS 요청하고 한 줄씩 출력
# 별도 터미널에서 부하 테스트하는 동안 띄워두면 실시간 모니터링
set -e
INTERVAL=${1:-1}
echo "▶ 데몬 STATS 모니터링 (간격: ${INTERVAL}s, Ctrl+C로 종료)"
while true; do
    LINE=$(printf 'STATS\n' | timeout 2 nc -q1 127.0.0.1 9090 2>/dev/null | head -1 || true)
    TS=$(date '+%H:%M:%S')
    echo "[$TS] ${LINE:-(no response)}"
    sleep "$INTERVAL"
done
