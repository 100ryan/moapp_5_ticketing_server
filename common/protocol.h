#pragma once

#include <cstdint>
#include <cstddef>

namespace ticketing {

constexpr int MAX_SEATS = 5000;  // 콘서트홀급 (올림픽홀 5,000석 기준)

constexpr int DAEMON_PORT       = 9090;  // 데몬 (라우터, 외부 노출)
constexpr int MAIN_SERVER_PORT  = 9091;  // 예매 서버 (좌석 CAS, 내부)
constexpr int QUEUE_SERVER_PORT = 9092;  // 대기열 서버 (토큰/큐, 내부)

constexpr int TOKEN_TTL_SECONDS = 60;
constexpr int NONCE_TTL_SECONDS = 30;

constexpr int MIN_SLIDING_LOG_LEN = 10;

// 한 줄 ('\n' 단위) 최대 크기. 클라이언트가 '\n' 없이 무한 송신해서 서버 메모리를
// 무한히 잡아먹는 DoS 를 방지하기 위함. AUTH 가 가장 큰 요청인데 sliding_log 포함
// 2~3KB 수준이라 64KB 면 충분히 여유.
constexpr size_t MAX_LINE_BYTES = 64 * 1024;

// 토큰 버킷 (유량 제어)
// 5,000석 콘서트 + 동접 10만 시나리오 권장값 (경쟁률 20:1):
//   - RATE=500/s → 5,000석을 약 10초에 채울 수 있는 속도
//   - CAP=1000   → 일시 폭주 1,000명까지 한꺼번에 받음
constexpr int    BUCKET_CAPACITY = 1000;       // 초기/최대 보유 토큰
constexpr double BUCKET_REFILL_PER_SEC = 500;  // 초당 발급 한도
constexpr int    QUEUE_TTL_SECONDS = 300;      // 큐 항목 만료 (대기열 길어질 수 있음)

constexpr const char* TICKET_FILE = "data/seats.bin";

constexpr const char* DAEMON_SECRET = "moapp-daemon-secret-key-2026";

}
