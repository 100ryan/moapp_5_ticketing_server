// 데몬 (미들웨어)
// - 슬라이딩 인증 검증
// - HMAC 토큰 발급 (토큰 버킷으로 유량 제어, 부족하면 대기열로)
// - 메인 서버로 RESERVE/FETCH_STATUS 포워딩
//
// 프로토콜 (앱 <-> 데몬):
//   HELLO:<device_id>                                            -> NONCE:<hex>
//   AUTH:<user_id>:<device_id>:<nonce>:<sliding_log>:<hash>      -> TOKEN:<...>           (즉시 발급)
//                                                                -> QUEUED:<pos>:<eta>   (대기)
//                                                                -> AUTH_FAIL:<reason>
//   POLL:<device_id>                                             -> TOKEN:<...> | QUEUED:<pos>:<eta> | POLL_FAIL:<reason>
//   RESERVE:<token>:<seat_idx>                                   -> RESULT:SUCCESS | FAIL_TAKEN | BAD_TOKEN
//   FETCH_STATUS                                                 -> STATUS:<200문자>
//   STATS                                                        -> STATS:k=v k=v ...

#include "../common/protocol.h"
#include "../common/sha256.h"
#include "../common/hmac.h"
#include "../common/netutil.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

using namespace ticketing;
using clk = std::chrono::steady_clock;

struct Nonce { std::string value; time_t issued_at; };

static std::unordered_map<std::string, Nonce> g_nonces;
static int g_main_fd = -1;
static std::string g_main_recv_buf;

// =============== Stats =======================================================
struct Stats {
    long active_conn = 0;
    long total_conn  = 0;
    long nonces_issued = 0;
    long tokens_direct = 0;
    long tokens_queued = 0;
    long auth_fail = 0;
    long reserve_success = 0;
    long reserve_fail_taken = 0;
    long reserve_bad_token = 0;
    long peak_active = 0;
    long peak_queue = 0;
};
static Stats g_stats;
static clk::time_point g_last_print = clk::now();

// =============== Runtime params (env override) ===============================
static int    g_cap;          // 버킷 capacity
static double g_rate;         // 초당 발급 한도
static int    g_queue_ttl;    // 큐 항목 TTL (초)

static int env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}
static double env_dbl(const char* name, double def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atof(v) : def;
}

// =============== Token Bucket ================================================
struct TokenBucket {
    double tokens;
    clk::time_point last_refill;

    TokenBucket() : tokens(0), last_refill(clk::now()) {}
    void init() { tokens = g_cap; last_refill = clk::now(); }

    void refill() {
        auto now = clk::now();
        double dt = std::chrono::duration<double>(now - last_refill).count();
        last_refill = now;
        tokens += dt * g_rate;
        if (tokens > g_cap) tokens = g_cap;
    }

    bool try_consume() {
        refill();
        if (tokens >= 1.0) { tokens -= 1.0; return true; }
        return false;
    }
};

static TokenBucket g_bucket;

// =============== Wait Queue ==================================================
// 같은 device_id 는 한 번만. ready_token 가 비어 있으면 아직 대기.
struct QueueEntry {
    std::string device_id;
    std::string user_id;
    std::string ready_token;
    time_t      enqueued_at;
};

static std::vector<QueueEntry> g_queue;

static int find_in_queue(const std::string& device_id) {
    for (size_t i = 0; i < g_queue.size(); i++)
        if (g_queue[i].device_id == device_id) return int(i);
    return -1;
}

// 만료된 큐 항목 제거 (앱이 끝까지 POLL 안 한 경우)
static void evict_stale() {
    time_t now = std::time(nullptr);
    for (auto it = g_queue.begin(); it != g_queue.end(); ) {
        if (now - it->enqueued_at > g_queue_ttl) it = g_queue.erase(it);
        else ++it;
    }
}

static std::string make_token(const std::string& user_id);

// 큐 앞에서부터 토큰을 채워준다 (버킷 한계까지)
static void drain_queue() {
    evict_stale();
    for (auto& e : g_queue) {
        if (!e.ready_token.empty()) continue;
        if (!g_bucket.try_consume()) break;
        e.ready_token = make_token(e.user_id);
    }
}

static int eta_for_pos(int pos) {
    if (g_rate <= 0) return 9999;
    return int((pos + 1) / g_rate);
}

// =============================================================================

static std::string gen_nonce_hex() {
    static std::random_device rd;
    static std::mt19937_64 rng(rd());
    uint64_t v = rng();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx", (unsigned long)v);
    return std::string(buf);
}

static bool ensure_main_connection() {
    if (g_main_fd >= 0) return true;
    g_main_fd = connect_tcp("127.0.0.1", MAIN_SERVER_PORT);
    if (g_main_fd < 0) {
        std::cerr << "[데몬] 메인 서버 연결 실패\n";
        return false;
    }
    g_main_recv_buf.clear();
    std::cerr << "[데몬] 메인 서버 연결 OK\n";
    return true;
}

static std::string main_request(const std::string& req) {
    if (!ensure_main_connection()) return "RESULT:BAD_REQUEST";
    if (!send_line(g_main_fd, req)) {
        ::close(g_main_fd); g_main_fd = -1;
        return "RESULT:BAD_REQUEST";
    }
    while (true) {
        size_t pos = g_main_recv_buf.find('\n');
        if (pos != std::string::npos) {
            std::string line = g_main_recv_buf.substr(0, pos);
            g_main_recv_buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        char buf[2048];
        ssize_t r = ::read(g_main_fd, buf, sizeof(buf));
        if (r <= 0) {
            ::close(g_main_fd); g_main_fd = -1;
            return "RESULT:BAD_REQUEST";
        }
        g_main_recv_buf.append(buf, buf + r);
    }
}

static std::string make_token(const std::string& user_id) {
    time_t exp = std::time(nullptr) + TOKEN_TTL_SECONDS;
    std::string payload = user_id + "|" + std::to_string((long long)exp);
    std::string mac = hmac_sha256_hex(DAEMON_SECRET, payload);
    return payload + "." + mac;
}

static std::string verify_token(const std::string& token) {
    size_t dot = token.rfind('.');
    if (dot == std::string::npos) return "";
    std::string payload = token.substr(0, dot);
    std::string mac = token.substr(dot + 1);
    if (hmac_sha256_hex(DAEMON_SECRET, payload) != mac) return "";
    size_t bar = payload.find('|');
    if (bar == std::string::npos) return "";
    std::string user_id = payload.substr(0, bar);
    long long exp = std::atoll(payload.c_str() + bar + 1);
    if (std::time(nullptr) > exp) return "";
    return user_id;
}

static std::string handle_hello(const std::vector<std::string>& parts) {
    if (parts.size() < 2 || parts[1].empty()) return "AUTH_FAIL:bad_request";
    std::string device_id = parts[1];
    std::string nonce = gen_nonce_hex();
    g_nonces[device_id] = { nonce, std::time(nullptr) };
    g_stats.nonces_issued++;
    return "NONCE:" + nonce;
}

static std::string handle_auth(const std::vector<std::string>& parts) {
    if (parts.size() < 6) return "AUTH_FAIL:bad_request";
    const std::string& user_id     = parts[1];
    const std::string& device_id   = parts[2];
    const std::string& nonce       = parts[3];
    const std::string& sliding_log = parts[4];
    const std::string& client_hash = parts[5];

    if (user_id.empty() || device_id.empty()) { g_stats.auth_fail++; return "AUTH_FAIL:bad_request"; }
    if (sliding_log.size() < MIN_SLIDING_LOG_LEN) { g_stats.auth_fail++; return "AUTH_FAIL:no_sliding"; }

    auto it = g_nonces.find(device_id);
    if (it == g_nonces.end()) { g_stats.auth_fail++; return "AUTH_FAIL:unknown_nonce"; }
    if (it->second.value != nonce) { g_stats.auth_fail++; return "AUTH_FAIL:nonce_mismatch"; }
    if (std::time(nullptr) - it->second.issued_at > NONCE_TTL_SECONDS) {
        g_nonces.erase(it);
        g_stats.auth_fail++;
        return "AUTH_FAIL:nonce_expired";
    }

    std::string canonical = user_id + "|" + device_id + "|" + nonce + "|" + sliding_log;
    std::string expected = Sha256::hash_hex(canonical);
    if (expected != client_hash) { g_stats.auth_fail++; return "AUTH_FAIL:hash_mismatch"; }

    g_nonces.erase(it);

    // 검증 통과 -> 토큰 버킷으로 유량 제어
    drain_queue();  // 먼저 대기자 처리
    if (g_queue.empty() && g_bucket.try_consume()) {
        g_stats.tokens_direct++;
        return "TOKEN:" + make_token(user_id);
    }

    // 큐로
    int idx = find_in_queue(device_id);
    if (idx < 0) {
        g_queue.push_back({ device_id, user_id, "", std::time(nullptr) });
        idx = int(g_queue.size()) - 1;
    } else {
        g_queue[idx].user_id = user_id;
    }
    if ((long)g_queue.size() > g_stats.peak_queue) g_stats.peak_queue = (long)g_queue.size();
    return "QUEUED:" + std::to_string(idx) + ":" + std::to_string(eta_for_pos(idx));
}

static std::string handle_poll(const std::vector<std::string>& parts) {
    if (parts.size() < 2 || parts[1].empty()) return "POLL_FAIL:bad_request";
    drain_queue();
    int idx = find_in_queue(parts[1]);
    if (idx < 0) return "POLL_FAIL:not_in_queue";
    if (!g_queue[idx].ready_token.empty()) {
        std::string t = g_queue[idx].ready_token;
        g_queue.erase(g_queue.begin() + idx);
        g_stats.tokens_queued++;
        return "TOKEN:" + t;
    }
    return "QUEUED:" + std::to_string(idx) + ":" + std::to_string(eta_for_pos(idx));
}

static std::string handle_reserve(const std::string& line) {
    size_t first = line.find(':');
    size_t second = line.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos) return "RESULT:BAD_REQUEST";
    std::string token = line.substr(first + 1, second - first - 1);
    int seat = std::atoi(line.c_str() + second + 1);

    std::string user = verify_token(token);
    if (user.empty()) { g_stats.reserve_bad_token++; return "RESULT:BAD_TOKEN"; }
    if (seat < 0 || seat >= MAX_SEATS) return "RESULT:BAD_REQUEST";

    std::string r = main_request("RESERVE:" + std::to_string(seat));
    if (r == "RESULT:SUCCESS") g_stats.reserve_success++;
    else if (r == "RESULT:FAIL_TAKEN") g_stats.reserve_fail_taken++;
    return r;
}

static std::string stats_line() {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "active=%ld total_conn=%ld nonces=%ld tok_direct=%ld tok_queue=%ld "
        "queue=%ld peak_queue=%ld peak_active=%ld reserves=%ld ileseat=%ld macros_blocked=%ld auth_fail=%ld",
        g_stats.active_conn, g_stats.total_conn, g_stats.nonces_issued,
        g_stats.tokens_direct, g_stats.tokens_queued,
        (long)g_queue.size(), g_stats.peak_queue, g_stats.peak_active,
        g_stats.reserve_success, g_stats.reserve_fail_taken,
        g_stats.reserve_bad_token, g_stats.auth_fail);
    return std::string(buf);
}

static void maybe_print_stats() {
    auto now = clk::now();
    if (std::chrono::duration<double>(now - g_last_print).count() < 1.0) return;
    g_last_print = now;
    std::cout << "[stats] " << stats_line() << std::endl;
}

static std::string dispatch(const std::string& line) {
    if (line.rfind("HELLO:", 0) == 0)        return handle_hello(split(line, ':'));
    if (line.rfind("AUTH:", 0) == 0)         return handle_auth(split(line, ':'));
    if (line.rfind("POLL:", 0) == 0)         return handle_poll(split(line, ':'));
    if (line.rfind("RESERVE:", 0) == 0)      return handle_reserve(line);
    if (line == "FETCH_STATUS")              return main_request("FETCH_STATUS");
    if (line == "STATS")                     return "STATS:" + stats_line();
    return "AUTH_FAIL:unknown_cmd";
}

int main() {
    g_cap       = env_int("BUCKET_CAP",  BUCKET_CAPACITY);
    g_rate      = env_dbl("BUCKET_RATE", BUCKET_REFILL_PER_SEC);
    g_queue_ttl = env_int("QUEUE_TTL",   QUEUE_TTL_SECONDS);
    g_bucket.init();

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::perror("socket"); return 1; }
    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DAEMON_PORT);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    if (::listen(server_fd, SOMAXCONN) < 0) {
        std::perror("listen"); return 1;
    }

    int epfd = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    std::unordered_map<int, std::string> client_buf;

    ensure_main_connection();

    std::cout << "[데몬] 0.0.0.0:" << DAEMON_PORT << " 가동"
              << " (버킷 cap=" << g_cap
              << " rate=" << g_rate << "/s"
              << " queue_ttl=" << g_queue_ttl << "s)\n";

    epoll_event events[128];
    while (true) {
        int n = ::epoll_wait(epfd, events, 128, 500); // 0.5초마다 깨워서 drain
        if (n == 0) { drain_queue(); maybe_print_stats(); continue; }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == server_fd) {
                int cfd = ::accept(server_fd, nullptr, nullptr);
                if (cfd < 0) continue;
                epoll_event cev{};
                cev.events = EPOLLIN;
                cev.data.fd = cfd;
                ::epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                client_buf[cfd] = "";
                g_stats.active_conn++;
                g_stats.total_conn++;
                if (g_stats.active_conn > g_stats.peak_active)
                    g_stats.peak_active = g_stats.active_conn;
                continue;
            }

            char buf[2048];
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r <= 0) {
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                client_buf.erase(fd);
                g_stats.active_conn--;
                continue;
            }
            std::string& acc = client_buf[fd];
            acc.append(buf, buf + r);

            size_t pos;
            bool drop = false;
            while ((pos = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                std::string resp = dispatch(line);
                if (!send_line(fd, resp)) { drop = true; break; }
            }
            if (drop) {
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                client_buf.erase(fd);
                g_stats.active_conn--;
            }
        }

        drain_queue();      // 매 wake마다 큐 처리
        maybe_print_stats(); // 1초마다 통계 한 줄
    }
    return 0;
}
