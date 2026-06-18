#!/usr/bin/env python3
"""
페이싱 부하 테스트 — 시연 중 끼어들기 가능 버전.

용도:
  - 한 번에 N명 들이는 게 아니라 초당 RATE 명씩 차례로 흘려서, 시연자가 폰에서
    자유롭게 한 자리 끼어들 수 있게 틈을 둠.
  - 시작 시 좌석 파일 자동 초기화 (./tools/init_ticket_file)

사용법:
    python3 tools/loadtest_paced.py
       → 정상 100,000 + 매크로 20,000, 초당 300명 (약 6.7분)

    python3 tools/loadtest_paced.py 100000 20000 300
    python3 tools/loadtest_paced.py 50000 10000 500     # 더 짧고 빠르게
    python3 tools/loadtest_paced.py 100000 20000 100    # 더 길게 (20분, 천천히)

도중에 Ctrl+C 누르면 즉시 멈춤. 폰 시연은 아무 때나 자유.
"""
import socket, hashlib, time, threading, sys, random, os, subprocess, signal

# 스레드당 스택을 줄여서 수천 스레드도 띄울 수 있게 (기본 8MB → 128KB)
threading.stack_size(128 * 1024)

DAEMON_HOST = '127.0.0.1'
DAEMON_PORT = 9090
MAX_SEATS   = 5000

# 동시 활성 워커 상한 — 큐가 길어져도 메모리 폭주 안 하게 막음
MAX_CONCURRENT = 3000
# 대기열에서 토큰 받기까지 최대 대기 (초)
QUEUE_POLL_MAX_SEC = 30
SOCKET_TIMEOUT = 10

stats_lock = threading.Lock()
stats = {
    'spawned'             : 0,
    'reserve_success'     : 0,
    'reserve_fail_taken'  : 0,
    'reserve_bad_token'   : 0,
    'queued'              : 0,
    'queue_received_token': 0,
    'queue_expired'       : 0,
    'auth_fail'           : 0,
    'sold_out'            : 0,
    'err'                 : 0,
    'active'              : 0,
}
g_stop = threading.Event()
concurrency_sem = threading.BoundedSemaphore(MAX_CONCURRENT)

def add(k, n=1):
    with stats_lock:
        stats[k] += n

def snap():
    with stats_lock:
        return stats.copy()

def call(line):
    """한 줄 요청 → 한 줄 응답."""
    payload = (line + '\n').encode()
    with socket.socket() as s:
        s.settimeout(SOCKET_TIMEOUT)
        s.connect((DAEMON_HOST, DAEMON_PORT))
        s.sendall(payload)
        buf = b''
        while b'\n' not in buf:
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
    return buf.decode(errors='replace').strip().split('\n')[0]

def normal_user(i):
    add('active'); add('spawned')
    try:
        uid = f'user-{i}'
        dev = f'dev-{i}-{random.randint(0, 10**9)}'
        seat = random.randint(0, MAX_SEATS - 1)

        r = call(f'HELLO:{dev}')
        if r.startswith('SOLD_OUT'): add('sold_out'); return
        if not r.startswith('NONCE:'): add('err'); return
        nonce = r[6:]

        sliding_log = ';'.join(
            f'{x},0,{t}' for x, t in zip(range(0, 200, 10), range(0, 2000, 100))
        )
        canonical = f'{uid}|{dev}|{nonce}|{sliding_log}'
        h = hashlib.sha256(canonical.encode()).hexdigest()
        r = call(f'AUTH:{uid}:{dev}:{nonce}:{sliding_log}:{h}')

        if r.startswith('SOLD_OUT'): add('sold_out'); return

        token = None
        if r.startswith('TOKEN:'):
            token = r[6:]
        elif r.startswith('QUEUED:'):
            add('queued')
            for _ in range(QUEUE_POLL_MAX_SEC):
                if g_stop.is_set(): return
                time.sleep(1)
                pr = call(f'POLL:{dev}')
                if pr.startswith('SOLD_OUT'): add('sold_out'); return
                if pr.startswith('TOKEN:'):
                    token = pr[6:]
                    add('queue_received_token')
                    break
                if pr.startswith('POLL_FAIL'):
                    add('queue_expired'); return
        else:
            add('auth_fail'); return

        if not token: add('err'); return

        r = call(f'RESERVE:{token}:{seat}')
        if r == 'RESULT:SUCCESS':       add('reserve_success')
        elif r == 'RESULT:FAIL_TAKEN':  add('reserve_fail_taken')
        elif r == 'RESULT:BAD_TOKEN':   add('reserve_bad_token')
        else:                            add('err')
    except Exception:
        add('err')
    finally:
        add('active', -1)
        concurrency_sem.release()

def macro_user(i):
    add('active'); add('spawned')
    try:
        fake = f'attacker-{i}|9999999999.deadbeef'
        seat = random.randint(0, MAX_SEATS - 1)
        r = call(f'RESERVE:{fake}:{seat}')
        if r == 'RESULT:BAD_TOKEN': add('reserve_bad_token')
        else:                       add('err')
    except Exception:
        add('err')
    finally:
        add('active', -1)
        concurrency_sem.release()

def progress_printer(total, t0):
    last_spawned = 0
    while not g_stop.is_set():
        time.sleep(2)
        s = snap()
        dt = time.perf_counter() - t0
        spawn_rps = (s['spawned'] - last_spawned) / 2.0
        last_spawned = s['spawned']
        succ_rate = s['reserve_success']
        eta_min = ((total - s['spawned']) / max(spawn_rps, 1)) / 60.0 if s['spawned'] < total else 0
        print(
            f"[{dt:6.0f}s] spawn={s['spawned']:>7,}/{total:,} "
            f"active={s['active']:>5,}  "
            f"성공={succ_rate:>5,}  매크로차단={s['reserve_bad_token']:>6,}  "
            f"대기={s['queued']:>5,}→토큰={s['queue_received_token']:>4,}  만료={s['queue_expired']:>4,}  "
            f"매진={s['sold_out']:>5,}  에러={s['err']:>4,}  "
            f"(spawn {spawn_rps:.0f}/s, ETA {eta_min:.1f}분)",
            flush=True
        )

def init_seats():
    if not os.path.exists('tools/init_ticket_file'):
        print("[init] tools/init_ticket_file 못 찾음 — ticketing 루트에서 실행해주세요")
        sys.exit(1)
    print("[init] 좌석 파일 초기화...")
    rc = subprocess.run(['./tools/init_ticket_file']).returncode
    if rc != 0:
        print(f"[init] FAILED (rc={rc})")
        sys.exit(1)
    print("[init] OK")

def handle_sigint(signum, frame):
    print("\n[중단] Ctrl+C 받음 — 정리 중...", flush=True)
    g_stop.set()

def main():
    argv = sys.argv[1:]
    n_normal = int(argv[0]) if len(argv) >= 1 else 100000
    n_macro  = int(argv[1]) if len(argv) >= 2 else 20000
    rate     = float(argv[2]) if len(argv) >= 3 else 300.0

    total = n_normal + n_macro
    dt = 1.0 / rate if rate > 0 else 0
    est_min = total / rate / 60.0

    signal.signal(signal.SIGINT, handle_sigint)

    init_seats()
    print()
    print(f"[설정] 정상 {n_normal:,} + 매크로 {n_macro:,} = 총 {total:,}")
    print(f"       초당 {rate:.0f}명 spawn / 동시 활성 상한 {MAX_CONCURRENT:,}명")
    print(f"       예상 spawn 시간: {est_min:.1f}분 + 큐 비울 시간")
    print(f"       대상: {DAEMON_HOST}:{DAEMON_PORT}")
    print(f"[안내] 폰에서 자유롭게 끼어드세요. Ctrl+C 로 즉시 중단.")
    print()

    # 정상/매크로 섞기
    jobs = [('macro', i) for i in range(n_macro)] + [('normal', i) for i in range(n_normal)]
    random.shuffle(jobs)

    t0 = time.perf_counter()
    pt = threading.Thread(target=progress_printer, args=(total, t0), daemon=True)
    pt.start()

    threads = []
    spawned = 0
    for kind, i in jobs:
        if g_stop.is_set(): break

        ideal = t0 + spawned * dt
        sleep_for = ideal - time.perf_counter()
        if sleep_for > 0:
            # g_stop 응답성 유지 — 작게 쪼개서 대기
            end = time.perf_counter() + sleep_for
            while not g_stop.is_set() and time.perf_counter() < end:
                time.sleep(min(0.1, end - time.perf_counter()))
            if g_stop.is_set(): break

        # 동시 접속 상한 — 블록 (자연스럽게 spawn rate 도 강제됨)
        if not concurrency_sem.acquire(timeout=5):
            continue  # 풀 가득찼는데 5초 안에 안 풀림 → 일단 패스

        target = macro_user if kind == 'macro' else normal_user
        t = threading.Thread(target=target, args=(i,), daemon=True)
        t.start()
        threads.append(t)
        spawned += 1

    print()
    print("[spawn 종료] 남은 워커 정리 중...")
    deadline = time.perf_counter() + QUEUE_POLL_MAX_SEC + 5
    for t in threads:
        remaining = deadline - time.perf_counter()
        if remaining <= 0: break
        t.join(timeout=remaining)

    g_stop.set()
    elapsed = time.perf_counter() - t0
    s = snap()
    print()
    print(f"▶ 완료 ({elapsed:.1f}s = {elapsed/60:.2f}분)")
    print(f"  spawn 횟수                  : {s['spawned']:>9,}")
    print(f"  정상 예매 성공              : {s['reserve_success']:>9,}")
    print(f"  정상 CAS 충돌(이선좌)       : {s['reserve_fail_taken']:>9,}")
    print(f"  대기열 진입 / 토큰 회수     : {s['queued']:>9,} / {s['queue_received_token']:,}")
    print(f"  대기열 만료 폐기            : {s['queue_expired']:>9,}")
    print(f"  매크로 차단 (BAD_TOKEN)     : {s['reserve_bad_token']:>9,}")
    print(f"  매진(SOLD_OUT) 응답         : {s['sold_out']:>9,}")
    print(f"  AUTH 실패                  : {s['auth_fail']:>9,}")
    print(f"  에러                        : {s['err']:>9,}")

if __name__ == '__main__':
    main()
