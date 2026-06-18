#!/usr/bin/env bash
# 자동 측정 스크립트 (강의 8강 성능분석 도구 통합) — 3-tier 버전
#
# 한 번 실행하면:
#   1. -pg -O0 -g 로 재빌드
#   2. seats.bin 초기화
#   3. main_server + queue_server + daemon 백그라운드 시작
#   4. strace -c 로 syscall 분포 캡처 (세 프로세스 모두)
#   5. loadtest.py 로 부하 (정상 200 + 매크로 800)
#   6. SIGTERM 으로 graceful shutdown (gmon.out 작성)
#   7. gprof 분석 결과 / strace 결과 / loadtest 결과를 한 화면에 정리
#
# 사용:
#   bash tools/profile.sh                  # default 시나리오
#   bash tools/profile.sh 500 2000         # 정상 500 + 매크로 2000

set -e
cd "$(dirname "$0")/.."

NORMAL=${1:-200}
MACRO=${2:-800}
RAMP=${3:-0}

REPORT_DIR=profile_report
mkdir -p $REPORT_DIR

# 이전 실행에서 안 죽은 프로세스 정리
echo "▶ Step 0/7  이전 실행 잔존 프로세스 정리"
pkill -9 -f main_server/main_server   2>/dev/null && echo "  - 기존 main_server 종료" || true
pkill -9 -f queue_server/queue_server 2>/dev/null && echo "  - 기존 queue_server 종료" || true
pkill -9 -f daemon/daemon              2>/dev/null && echo "  - 기존 daemon 종료" || true
sleep 0.3

# 스크립트 중간에 빠져나가도 백그라운드 프로세스가 살아남지 않게 trap
cleanup() {
    [ -n "${MAIN_PID:-}" ]   && kill -TERM $MAIN_PID   2>/dev/null || true
    [ -n "${QUEUE_PID:-}" ]  && kill -TERM $QUEUE_PID  2>/dev/null || true
    [ -n "${DAEMON_PID:-}" ] && kill -TERM $DAEMON_PID 2>/dev/null || true
    [ -n "${STRACE_MAIN:-}" ]   && kill -INT $STRACE_MAIN   2>/dev/null || true
    [ -n "${STRACE_QUEUE:-}" ]  && kill -INT $STRACE_QUEUE  2>/dev/null || true
    [ -n "${STRACE_DAEMON:-}" ] && kill -INT $STRACE_DAEMON 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "================================================================"
echo "▶ Step 1/7  -pg 빌드"
echo "================================================================"
make profile

echo ""
echo "================================================================"
echo "▶ Step 2/7  seats.bin 초기화"
echo "================================================================"
rm -f data/seats.bin
./tools/init_ticket_file

echo ""
echo "================================================================"
echo "▶ Step 3/7  서버 시작 (예매 → 대기열 → 데몬)"
echo "================================================================"
# gmon.out 이 서로 덮어쓰지 않게 GMON_OUT_PREFIX 로 파일명 분리
rm -f gmon.main.* gmon.queue.* gmon.daemon.* gmon.out

GMON_OUT_PREFIX=gmon.main   ./main_server/main_server   > $REPORT_DIR/main.log   2>&1 &
MAIN_PID=$!
sleep 0.3
GMON_OUT_PREFIX=gmon.queue  ./queue_server/queue_server > $REPORT_DIR/queue.log  2>&1 &
QUEUE_PID=$!
sleep 0.3
GMON_OUT_PREFIX=gmon.daemon ./daemon/daemon              > $REPORT_DIR/daemon.log 2>&1 &
DAEMON_PID=$!
sleep 0.5

echo "  main_server  PID = $MAIN_PID"
echo "  queue_server PID = $QUEUE_PID"
echo "  daemon       PID = $DAEMON_PID"

# 살아있는지 확인
for name_pid in "main_server:$MAIN_PID" "queue_server:$QUEUE_PID" "daemon:$DAEMON_PID"; do
    name=${name_pid%:*}
    pid=${name_pid#*:}
    if ! kill -0 $pid 2>/dev/null; then
        echo "✗ $name 가 떴다가 죽었음. 로그:"
        cat $REPORT_DIR/${name%_server}.log 2>/dev/null || cat $REPORT_DIR/${name}.log
        exit 1
    fi
done

echo ""
echo "================================================================"
echo "▶ Step 4/7  strace -c 부착 (syscall 분포 캡처)"
echo "================================================================"
STRACE_OK=1
if strace -c -p $MAIN_PID -o $REPORT_DIR/main.strace.txt 2>/dev/null &
then
    STRACE_MAIN=$!
    strace -c -p $QUEUE_PID  -o $REPORT_DIR/queue.strace.txt  2>/dev/null &
    STRACE_QUEUE=$!
    strace -c -p $DAEMON_PID -o $REPORT_DIR/daemon.strace.txt 2>/dev/null &
    STRACE_DAEMON=$!
    sleep 0.3
    if ! kill -0 $STRACE_MAIN 2>/dev/null; then
        echo "  ⚠ strace 부착 실패 (ptrace 권한 부족). 건너뜀."
        echo "    해결: sudo sysctl -w kernel.yama.ptrace_scope=0"
        STRACE_MAIN=""; STRACE_QUEUE=""; STRACE_DAEMON=""
        STRACE_OK=0
    else
        echo "  strace 부착 OK (3개 프로세스)"
    fi
else
    echo "  ⚠ strace 실행 실패. 건너뜀."
    STRACE_MAIN=""; STRACE_QUEUE=""; STRACE_DAEMON=""
    STRACE_OK=0
fi

echo ""
echo "================================================================"
echo "▶ Step 5/7  부하 테스트 (정상 $NORMAL + 매크로 $MACRO)"
echo "================================================================"
python3 tools/loadtest.py $NORMAL $MACRO $RAMP | tee $REPORT_DIR/loadtest.txt

echo ""
echo "================================================================"
echo "▶ Step 6/7  서버 graceful shutdown -> gmon.out 작성"
echo "================================================================"
if [ "$STRACE_OK" = "1" ]; then
    kill -INT $STRACE_MAIN $STRACE_QUEUE $STRACE_DAEMON 2>/dev/null || true
    sleep 0.3
fi
kill -TERM $MAIN_PID $QUEUE_PID $DAEMON_PID 2>/dev/null || true
wait $MAIN_PID $QUEUE_PID $DAEMON_PID 2>/dev/null || true
sleep 0.3

GMON_MAIN=$(ls -t gmon.main.*   2>/dev/null | head -1)
GMON_QUEUE=$(ls -t gmon.queue.*  2>/dev/null | head -1)
GMON_DAEMON=$(ls -t gmon.daemon.* 2>/dev/null | head -1)

if [ -n "$GMON_MAIN" ] && [ -f "$GMON_MAIN" ]; then
    gprof main_server/main_server "$GMON_MAIN" > $REPORT_DIR/main.gprof.txt 2>&1
    echo "  ✓ main.gprof.txt   생성  ($GMON_MAIN)"
else
    echo "  ✗ gmon.main.* 없음"
fi
if [ -n "$GMON_QUEUE" ] && [ -f "$GMON_QUEUE" ]; then
    gprof queue_server/queue_server "$GMON_QUEUE" > $REPORT_DIR/queue.gprof.txt 2>&1
    echo "  ✓ queue.gprof.txt  생성  ($GMON_QUEUE)"
else
    echo "  ✗ gmon.queue.* 없음"
fi
if [ -n "$GMON_DAEMON" ] && [ -f "$GMON_DAEMON" ]; then
    gprof daemon/daemon "$GMON_DAEMON" > $REPORT_DIR/daemon.gprof.txt 2>&1
    echo "  ✓ daemon.gprof.txt 생성  ($GMON_DAEMON)"
else
    echo "  ✗ gmon.daemon.* 없음"
fi

echo ""
echo "================================================================"
echo "▶ Step 7/7  요약 출력 (전체 결과는 $REPORT_DIR/ 안에)"
echo "================================================================"

for tag in main queue daemon; do
    file=$REPORT_DIR/$tag.gprof.txt
    echo ""
    echo "─── gprof: $tag (상위 함수) ─────────────────────────────────"
    if [ -f "$file" ]; then
        head -25 $file
    else
        echo "  (없음)"
    fi
done

for tag in main queue daemon; do
    file=$REPORT_DIR/$tag.strace.txt
    if [ -f "$file" ] && [ -s "$file" ]; then
        echo ""
        echo "─── strace -c: $tag (syscall 분포) ──────────────────────────"
        cat $file
    fi
done

echo ""
echo "================================================================"
echo "  완료. 전체 결과: $REPORT_DIR/"
echo "    - main.gprof.txt / queue.gprof.txt / daemon.gprof.txt  (함수별 self-time)"
echo "    - main.strace.txt / queue.strace.txt / daemon.strace.txt (syscall 분포)"
echo "    - loadtest.txt                                          (latency, RPS, bandwidth)"
echo "    - main.log / queue.log / daemon.log                      (서버 stdout)"
echo "================================================================"
