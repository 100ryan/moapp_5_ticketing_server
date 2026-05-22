#pragma once

#include <cstdint>

namespace ticketing {

constexpr int MAX_SEATS = 200;

constexpr int DAEMON_PORT = 9090;
constexpr int MAIN_SERVER_PORT = 9091;

constexpr int TOKEN_TTL_SECONDS = 60;
constexpr int NONCE_TTL_SECONDS = 30;

constexpr int MIN_SLIDING_LOG_LEN = 10;

// 토큰 버킷 (유량 제어)
constexpr int    BUCKET_CAPACITY = 5;        // 초기/최대 보유 토큰
constexpr double BUCKET_REFILL_PER_SEC = 5;  // 초당 발급 한도
constexpr int    QUEUE_TTL_SECONDS = 120;    // 큐 항목 만료

constexpr const char* TICKET_FILE = "data/seats.bin";

constexpr const char* DAEMON_SECRET = "moapp-daemon-secret-key-2026";

}
