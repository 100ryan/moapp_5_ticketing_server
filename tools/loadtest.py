#!/usr/bin/env python3
"""
부하 테스트 — 제안서 8.2 시나리오 + 강의 2강의 3축(latency, throughput, bandwidth) 측정.

- 정상 사용자: HELLO → AUTH → (TOKEN | QUEUED→POLL) → RESERVE
- 매크로 사용자: 인증 생략, 가짜 토큰으로 RESERVE → BAD_TOKEN 차단되어야 함

기본 비율은 매크로 80% + 정상 20% (제안서 8.2).

사용:
    python3 tools/loadtest.py                    # default: 정상 200 + 매크로 800
    python3 tools/loadtest.py 200 800            # 명시
    python3 tools/loadtest.py 200 800 3          # + ramp-up 3초

출력 추가 항목 (이전 버전 대비):
  - HELLO / AUTH / RESERVE 구간별 p50/p95/p99 분리
  - 정상 사용자 완료 throughput (RPS)
  - 전체 송수신 bytes -> bandwidth (KB/s)
  - 더 자세한 percentile (p50, p90, p95, p99, p99.9)
"""
import socket, hashlib, time, threading, sys, random

DAEMON_HOST = '127.0.0.1'
DAEMON_PORT = 9090
MAX_SEATS   = 5000   # protocol.h 와 일치시킬 것

stats = {
    'reserve_success'     : 0,
    'reserve_fail_taken'  : 0,
    'reserve_bad_token'   : 0,
    'queued'              : 0,
    'queue_received_token': 0,
    'queue_expired'       : 0,
    'auth_fail'           : 0,
    'sold_out'            : 0,   # 매진 응답 받고 즉시 종료
    'err'                 : 0,
    'lat_total'           : [],  # 정상 사용자 전체 (HELLO 시작 ~ RESERVE 끝)
    'lat_hello'           : [],
    'lat_auth'            : [],
    'lat_reserve'         : [],
    'bytes_sent'          : 0,
    'bytes_recv'          : 0,
}
lock = threading.Lock()

def call(line, timeout=10):
    """요청을 보내고 (응답, latency_ms, sent_bytes, recv_bytes) 를 반환."""
    payload = (line + '\n').encode()
    t0 = time.perf_counter()
    with socket.socket() as s:
        s.settimeout(timeout)
        s.connect((DAEMON_HOST, DAEMON_PORT))
        s.sendall(payload)
        buf = b''
        while b'\n' not in buf:
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
    lat_ms = (time.perf_counter() - t0) * 1000
    response = buf.decode(errors='replace').strip().split('\n')[0]
    return response, lat_ms, len(payload), len(buf)

def add_bytes(sent, recv):
    with lock:
        stats['bytes_sent'] += sent
        stats['bytes_recv'] += recv

def normal_user(i, seat):
    t0 = time.perf_counter()
    uid = f'user-{i}'
    dev = f'dev-{i}-{random.randint(0, 10**9)}'
    try:
        # ----- HELLO -----
        r, lat, sent, recv = call(f'HELLO:{dev}')
        add_bytes(sent, recv)
        with lock: stats['lat_hello'].append(lat)
        if r.startswith('SOLD_OUT'):
            with lock: stats['sold_out'] += 1
            return
        if not r.startswith('NONCE:'):
            with lock: stats['err'] += 1
            return
        nonce = r[6:]

        # ----- AUTH -----
        sliding_log = ';'.join(f'{x},{y},{t}' for x, y, t in
                               zip(range(0, 200, 10), [0]*20, range(0, 2000, 100)))
        canonical = f'{uid}|{dev}|{nonce}|{sliding_log}'
        h = hashlib.sha256(canonical.encode()).hexdigest()
        r, lat, sent, recv = call(f'AUTH:{uid}:{dev}:{nonce}:{sliding_log}:{h}')
        add_bytes(sent, recv)
        with lock: stats['lat_auth'].append(lat)

        if r.startswith('SOLD_OUT'):
            with lock: stats['sold_out'] += 1
            return

        token = None
        if r.startswith('TOKEN:'):
            token = r[6:]
        elif r.startswith('QUEUED:'):
            with lock: stats['queued'] += 1
            # 최대 120초까지 POLL — 단, SOLD_OUT 받으면 즉시 종료
            sold_out_break = False
            for _ in range(120):
                time.sleep(1)
                pr, plat, ps, pr2 = call(f'POLL:{dev}')
                add_bytes(ps, pr2)
                if pr.startswith('SOLD_OUT'):
                    with lock: stats['sold_out'] += 1
                    sold_out_break = True
                    break
                if pr.startswith('TOKEN:'):
                    token = pr[6:]
                    with lock: stats['queue_received_token'] += 1
                    break
                elif pr.startswith('POLL_FAIL'):
                    with lock: stats['queue_expired'] += 1
                    break
            if sold_out_break:
                return
        else:
            with lock: stats['auth_fail'] += 1
            return

        if not token:
            with lock: stats['err'] += 1
            return

        # ----- RESERVE -----
        r, lat, sent, recv = call(f'RESERVE:{token}:{seat}')
        add_bytes(sent, recv)
        with lock:
            stats['lat_reserve'].append(lat)
            if r == 'RESULT:SUCCESS':       stats['reserve_success'] += 1
            elif r == 'RESULT:FAIL_TAKEN':  stats['reserve_fail_taken'] += 1
            elif r == 'RESULT:BAD_TOKEN':   stats['reserve_bad_token'] += 1
            else:                            stats['err'] += 1
            stats['lat_total'].append((time.perf_counter() - t0) * 1000)
    except Exception:
        with lock: stats['err'] += 1

def macro_user(i, seat):
    try:
        fake_token = f'attacker-{i}|9999999999.deadbeef'
        r, lat, sent, recv = call(f'RESERVE:{fake_token}:{seat}')
        add_bytes(sent, recv)
        with lock:
            if r == 'RESULT:BAD_TOKEN': stats['reserve_bad_token'] += 1
            else: stats['err'] += 1
    except Exception:
        with lock: stats['err'] += 1

def pct(arr, p):
    if not arr: return 0.0
    s = sorted(arr)
    idx = min(int(len(s) * p), len(s) - 1)
    return s[idx]

def fmt_lat(arr, label):
    if not arr:
        return f'  {label:9s}: (no data)'
    return (f'  {label:9s}: n={len(arr):5d}  '
            f'avg={sum(arr)/len(arr):6.1f}  '
            f'p50={pct(arr,0.50):6.1f}  '
            f'p90={pct(arr,0.90):6.1f}  '
            f'p95={pct(arr,0.95):6.1f}  '
            f'p99={pct(arr,0.99):6.1f}  '
            f'p99.9={pct(arr,0.999):6.1f}  ms')

def main():
    argv = sys.argv[1:]
    n_normal = int(argv[0]) if len(argv) >= 1 else 200
    n_macro  = int(argv[1]) if len(argv) >= 2 else n_normal * 4   # 8:2 비율
    ramp_s   = float(argv[2]) if len(argv) >= 3 else 0.0

    total = n_normal + n_macro
    print(f'▶ 정상 {n_normal} + 매크로 {n_macro} (총 {total}, 비율 {n_normal*100/total:.0f}:{n_macro*100/total:.0f})')
    print(f'  대상: {DAEMON_HOST}:{DAEMON_PORT}, ramp-up: {ramp_s}s')
    print()

    jobs = []
    for i in range(n_macro):  jobs.append(('macro',  i))
    for i in range(n_normal): jobs.append(('normal', i))
    random.shuffle(jobs)

    t0 = time.perf_counter()
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

    elapsed = time.perf_counter() - t0

    # ============== 결과 출력 ==============
    print(f'▶ 완료 ({elapsed:.2f}s)')
    print(f'  정상 사용자 예매 성공          : {stats["reserve_success"]}')
    print(f'  정상 사용자 CAS 충돌(이선좌)    : {stats["reserve_fail_taken"]}')
    print(f'  대기열 진입 / 토큰 회수         : {stats["queued"]} / {stats["queue_received_token"]}')
    print(f'  대기열 만료 / 폐기              : {stats["queue_expired"]}')
    print(f'  매크로 차단 (BAD_TOKEN)        : {stats["reserve_bad_token"]}')
    print(f'  AUTH 실패                     : {stats["auth_fail"]}')
    print(f'  매진(SOLD_OUT)으로 즉시 종료    : {stats["sold_out"]}')
    print(f'  에러                          : {stats["err"]}')

    # ----- Throughput -----
    finished = stats['reserve_success'] + stats['reserve_fail_taken']
    rps_normal = finished / elapsed if elapsed > 0 else 0
    rps_all    = total / elapsed if elapsed > 0 else 0
    print()
    print(f'▶ Throughput (강의 2강)')
    print(f'  정상 사용자 완료 RPS           : {rps_normal:7.1f}  ({finished} req / {elapsed:.2f}s)')
    print(f'  전체 요청 RPS                  : {rps_all:7.1f}  ({total} req / {elapsed:.2f}s)')

    # ----- Bandwidth -----
    sent_kb_s = stats['bytes_sent'] / 1024 / elapsed if elapsed > 0 else 0
    recv_kb_s = stats['bytes_recv'] / 1024 / elapsed if elapsed > 0 else 0
    print()
    print(f'▶ Bandwidth (강의 2강)')
    print(f'  클라이언트 송신                : {stats["bytes_sent"]/1024:8.1f} KB  ({sent_kb_s:.1f} KB/s)')
    print(f'  클라이언트 수신                : {stats["bytes_recv"]/1024:8.1f} KB  ({recv_kb_s:.1f} KB/s)')

    # ----- Latency 구간별 -----
    print()
    print(f'▶ Latency (강의 2강) — 단계별 분리')
    print(fmt_lat(stats['lat_hello'],   'HELLO'))
    print(fmt_lat(stats['lat_auth'],    'AUTH'))
    print(fmt_lat(stats['lat_reserve'], 'RESERVE'))
    print(fmt_lat(stats['lat_total'],   'TOTAL'))

    # ----- 매크로 차단율 -----
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
