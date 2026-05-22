#!/usr/bin/env python3
"""
부하 테스트 — 제안서 8.2 시나리오 축소판.

- 정상 사용자: HELLO → AUTH → (TOKEN | QUEUED→POLL) → RESERVE
- 매크로 사용자: 인증 생략, 가짜 토큰으로 RESERVE → BAD_TOKEN 차단되어야 함

기본 비율은 매크로 80% + 정상 20% (제안서 8.2).

사용:
    python3 tools/loadtest.py                    # default: 정상 200 + 매크로 800
    python3 tools/loadtest.py 200 800            # 명시
    python3 tools/loadtest.py 200 800 3          # + ramp-up 3초 (한 번에 다 안 던지고 분산)
"""
import socket, hashlib, time, threading, sys, random

DAEMON_HOST = '127.0.0.1'
DAEMON_PORT = 9090
MAX_SEATS   = 200

stats = {
    'reserve_success'     : 0,
    'reserve_fail_taken'  : 0,
    'reserve_bad_token'   : 0,
    'queued'              : 0,
    'queue_received_token': 0,
    'queue_expired'       : 0,
    'auth_fail'           : 0,
    'err'                 : 0,
    'latency_ms'          : [],
}
lock = threading.Lock()

def call(line, timeout=10):
    with socket.socket() as s:
        s.settimeout(timeout)
        s.connect((DAEMON_HOST, DAEMON_PORT))
        s.sendall((line + '\n').encode())
        buf = b''
        while b'\n' not in buf:
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
        return buf.decode().strip().split('\n')[0]

def normal_user(i, seat):
    t0 = time.time()
    uid = f'user-{i}'
    dev = f'dev-{i}-{random.randint(0, 10**9)}'
    try:
        r = call(f'HELLO:{dev}')
        if not r.startswith('NONCE:'):
            with lock: stats['err'] += 1
            return
        nonce = r[6:]

        sliding_log = ';'.join(f'{x},{y},{t}' for x, y, t in
                               zip(range(0, 200, 10), [0]*20, range(0, 2000, 100)))
        canonical = f'{uid}|{dev}|{nonce}|{sliding_log}'
        h = hashlib.sha256(canonical.encode()).hexdigest()
        r = call(f'AUTH:{uid}:{dev}:{nonce}:{sliding_log}:{h}')

        token = None
        if r.startswith('TOKEN:'):
            token = r[6:]
        elif r.startswith('QUEUED:'):
            with lock: stats['queued'] += 1
            # 최대 120초까지 POLL
            for _ in range(120):
                time.sleep(1)
                pr = call(f'POLL:{dev}')
                if pr.startswith('TOKEN:'):
                    token = pr[6:]
                    with lock: stats['queue_received_token'] += 1
                    break
                elif pr.startswith('POLL_FAIL'):
                    with lock: stats['queue_expired'] += 1
                    break
        else:
            with lock: stats['auth_fail'] += 1
            return

        if not token:
            with lock: stats['err'] += 1
            return

        r = call(f'RESERVE:{token}:{seat}')
        with lock:
            if r == 'RESULT:SUCCESS':       stats['reserve_success'] += 1
            elif r == 'RESULT:FAIL_TAKEN':  stats['reserve_fail_taken'] += 1
            elif r == 'RESULT:BAD_TOKEN':   stats['reserve_bad_token'] += 1
            else:                            stats['err'] += 1
            stats['latency_ms'].append((time.time() - t0) * 1000)
    except Exception:
        with lock: stats['err'] += 1

def macro_user(i, seat):
    try:
        fake_token = f'attacker-{i}|9999999999.deadbeef'
        r = call(f'RESERVE:{fake_token}:{seat}')
        with lock:
            if r == 'RESULT:BAD_TOKEN': stats['reserve_bad_token'] += 1
            else: stats['err'] += 1
    except Exception:
        with lock: stats['err'] += 1

def main():
    argv = sys.argv[1:]
    n_normal = int(argv[0]) if len(argv) >= 1 else 200
    n_macro  = int(argv[1]) if len(argv) >= 2 else n_normal * 4   # 8:2 비율
    ramp_s   = float(argv[2]) if len(argv) >= 3 else 0.0

    total = n_normal + n_macro
    print(f'▶ 정상 {n_normal} + 매크로 {n_macro} (총 {total}, 비율 {n_normal*100/total:.0f}:{n_macro*100/total:.0f})')
    print(f'  대상: {DAEMON_HOST}:{DAEMON_PORT}, ramp-up: {ramp_s}s')

    # 발사 시퀀스(섞기). 매크로/정상이 시간상 분산되도록.
    jobs = []
    for i in range(n_macro):
        jobs.append(('macro', i))
    for i in range(n_normal):
        jobs.append(('normal', i))
    random.shuffle(jobs)

    t0 = time.time()
    threads = []
    delay_step = (ramp_s / total) if (ramp_s > 0 and total > 0) else 0

    for idx, (kind, i) in enumerate(jobs):
        seat = random.randint(0, MAX_SEATS - 1)
        target = macro_user if kind == 'macro' else normal_user
        t = threading.Thread(target=target, args=(i, seat))
        t.start()
        threads.append(t)
        if delay_step > 0:
            time.sleep(delay_step)

    for t in threads:
        t.join()

    elapsed = time.time() - t0
    lat = sorted(stats['latency_ms'])
    avg = sum(lat) / len(lat) if lat else 0
    p95 = lat[int(len(lat) * 0.95)] if lat else 0
    p99 = lat[int(len(lat) * 0.99)] if lat else 0

    print()
    print(f'▶ 완료 ({elapsed:.2f}s)')
    print(f'  정상 사용자 예매 성공          : {stats["reserve_success"]}')
    print(f'  정상 사용자 CAS 충돌(이선좌)    : {stats["reserve_fail_taken"]}')
    print(f'  대기열 진입 / 토큰 회수         : {stats["queued"]} / {stats["queue_received_token"]}')
    print(f'  대기열 만료 / 폐기              : {stats["queue_expired"]}')
    print(f'  매크로 차단 (BAD_TOKEN)        : {stats["reserve_bad_token"]}')
    print(f'  AUTH 실패                     : {stats["auth_fail"]}')
    print(f'  에러                          : {stats["err"]}')
    if lat:
        print(f'  정상 사용자 전체 흐름 지연(ms) : avg={avg:.1f}  p95={p95:.1f}  p99={p99:.1f}')

    print()
    if n_macro == 0:
        print('▶ 매크로 차단 검증 : 생략 (매크로 0명)')
    else:
        rate = stats['reserve_bad_token'] / n_macro
        print(f'▶ 매크로 차단율 : {rate*100:.1f}%')
        if rate >= 0.99:
            print('  ✓ 매크로 차단 OK')
        else:
            print('  ✗ 매크로 일부가 BAD_TOKEN 받지 못함')

if __name__ == '__main__':
    main()
