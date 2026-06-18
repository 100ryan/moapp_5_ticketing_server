#!/usr/bin/env python3
"""
좌석 파일 초기화 — 모드 선택 가능.

기본은 '모두 빈자리' (부하 테스트용 가장 깨끗한 상태).

사용법:
    python3 tools/init_seats.py                    # 5000석 모두 빈자리 (기본)
    python3 tools/init_seats.py empty              # 위와 동일
    python3 tools/init_seats.py taken              # 5000석 모두 매진 — SOLD_OUT 시나리오 테스트
    python3 tools/init_seats.py pattern            # i%7==0 매진 (기존 C++ tool 과 동일, 715/5000)
    python3 tools/init_seats.py random 1000        # 무작위 1,000석 매진

ticketing 디렉토리에서 실행해야 함 (data/seats.bin 상대경로).
"""
import struct, sys, os, random

MAX_SEATS = 5000
TICKET_FILE = 'data/seats.bin'

def write_seats(arr):
    if len(arr) != MAX_SEATS:
        raise ValueError(f'좌석 수 불일치: {len(arr)} != {MAX_SEATS}')
    os.makedirs(os.path.dirname(TICKET_FILE) or '.', exist_ok=True)
    with open(TICKET_FILE, 'wb') as f:
        f.write(struct.pack(f'{MAX_SEATS}i', *arr))

def main():
    argv = sys.argv[1:]
    mode = argv[0].lower() if len(argv) >= 1 else 'empty'

    if mode == 'empty':
        arr = [0] * MAX_SEATS
        taken = 0
    elif mode == 'taken':
        arr = [1] * MAX_SEATS
        taken = MAX_SEATS
    elif mode == 'pattern':
        arr = [1 if i % 7 == 0 else 0 for i in range(MAX_SEATS)]
        taken = sum(arr)
    elif mode == 'random':
        n = int(argv[1]) if len(argv) >= 2 else 500
        if n < 0 or n > MAX_SEATS:
            print(f'random 인자 범위 0..{MAX_SEATS} (입력: {n})')
            sys.exit(1)
        arr = [0] * MAX_SEATS
        for idx in random.sample(range(MAX_SEATS), n):
            arr[idx] = 1
        taken = n
    else:
        print(f'알 수 없는 모드: {mode!r}')
        print(__doc__)
        sys.exit(1)

    if not os.path.exists('tools'):
        print('[경고] ticketing 루트가 아닌 것 같습니다 — data/seats.bin 위치 확인하세요')

    write_seats(arr)
    free = MAX_SEATS - taken
    size = MAX_SEATS * 4
    print(f'[init] mode={mode}  매진={taken}  빈자리={free} / {MAX_SEATS}  ({size:,}바이트 -> {TICKET_FILE})')

if __name__ == '__main__':
    main()
