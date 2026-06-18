// 대기열 서버 (Queue Server) — 단일/멀티 스레드 통합 버전
//
// 모드 선택:
//   QUEUE_THREADS=1 (기본): 워커 1개 = 단일 스레드 reactor (락 경합 없음)
//   QUEUE_THREADS=N       : N개 워커 풀, 각자 epoll 보유, 라운드 로빈 분배
//
// 동시성 정책:
//   - 전역 상태 (g_queue / g_bucket / g_nonces / g_stats) → 단일 mutex 보호
//   - SHA-256 / HMAC 계산은 lock 밖 (CPU 병렬화 효과 최대화)
//   - epoll fd 는 워커마다 1개 (스레드 간 epoll 공유 안 함 → race 제거)
//   - accept 는 메인 스레드 단독, 새 fd 를 워커에게 epoll_ctl 로 이양
//
// 프로토콜 (데몬 ↔ 대기열 서버):
//   HELLO / AUTH / POLL / VERIFY / NOTIFY_SOLD_OUT / STATS

#include "../common/protocol.h"
#include "../common/sha256.h"
#include "../common/hmac.h"
#include "../common/netutil.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <random>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

using namespace ticketing;
using clk = std::chrono::steady_clock;

static volatile sig_atomic_t g_shutdown = 0;
static void on_signal(int) { g_shutdown = 1; }

static std::atomic<bool> g_sold_out{false};

// =============== 전역 상태 + 보호 mutex =====================================
static std::mutex g_state_mu;

struct Nonce { std::string value; time_t issued_at; };
static std::unordered_map<std::string, Nonce> g_nonces;  // 보호: g_state_mu

// 이미 사용된 토큰 (token -> 만료시각). 두 번째 VERIFY 는 BAD_TOKEN 으로 막아서
// 한 사용자가 한 토큰으로 여러 좌석 선점하는 시나리오 차단.
// 토큰 자체가 TTL 60초라 만료 지난 항목은 주기적으로 정리.
static std::unordered_map<std::string, time_t> g_used_tokens;  // 보호: g_state_mu

struct Stats {
    long active_conn = 0;
    long total_conn  = 0;
    long nonces_issued = 0;
    long tokens_direct = 0;
    long tokens_queued = 0;
    long auth_fail = 0;
    long verify_ok = 0;
    long verify_bad = 0;
    long verify_reuse = 0;  // 일회성 토큰 재사용 시도 (BAD_TOKEN 으로 막힘)
    long peak_active = 0;
    long peak_queue = 0;
};
static Stats g_stats;                                     // 보호: g_state_mu
static clk::time_point g_last_print = clk::now();         // 보호: g_state_mu

// =============== Runtime params (env override) ===============================
static int    g_cap;
static double g_rate;
static int    g_queue_ttl;
static int    g_n_threads;
// HMAC 시크릿. 운영 시 env 로 주입, 미설정 시 protocol.h 의 기본값.
// 하드코딩만 두면 소스 노출 시 토큰 위조 가능 — 배포 환경에선 반드시 env 로 덮는다.
static std::string g_secret;

static int env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}
static double env_dbl(const char* name, double def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atof(v) : def;
}

// =============== Token Bucket (보호: g_state_mu) =============================
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

static TokenBucket g_bucket;                              // 보호: g_state_mu

// =============== Wait Queue (보호: g_state_mu) ===============================
struct QueueEntry {
    std::string device_id;
    std::string user_id;
    std::string ready_token;
    time_t      enqueued_at;
};

static std::vector<QueueEntry> g_queue;                   // 보호: g_state_mu

// caller must hold g_state_mu
static int find_in_queue(const std::string& device_id) {
    for (size_t i = 0; i < g_queue.size(); i++)
        if (g_queue[i].device_id == device_id) return int(i);
    return -1;
}

// caller must hold g_state_mu
static void evict_stale() {
    time_t now = std::time(nullptr);
    for (auto it = g_queue.begin(); it != g_queue.end(); ) {
        if (now - it->enqueued_at > g_queue_ttl) it = g_queue.erase(it);
        else ++it;
    }
}

// caller must hold g_state_mu
// HELLO 만 보내고 AUTH 안 하는 device 가 g_nonces 에 영구히 쌓이는 누수를 막기 위함.
// 전체 맵 순회는 비용이 있어서 5초에 한 번만 청소.
static clk::time_point g_last_nonce_evict = clk::now();
static void evict_stale_nonces_locked() {
    auto now = clk::now();
    if (std::chrono::duration<double>(now - g_last_nonce_evict).count() < 5.0) return;
    g_last_nonce_evict = now;
    time_t t_now = std::time(nullptr);
    for (auto it = g_nonces.begin(); it != g_nonces.end(); ) {
        if (t_now - it->second.issued_at > NONCE_TTL_SECONDS) it = g_nonces.erase(it);
        else ++it;
    }
    // 사용된 토큰 — 토큰 exp 가 지나면 더 이상 검증 통과 못 하니 같이 GC
    for (auto it = g_used_tokens.begin(); it != g_used_tokens.end(); ) {
        if (t_now > it->second) it = g_used_tokens.erase(it);
        else ++it;
    }
}

// stateless — lock 불필요
static std::string make_token(const std::string& user_id) {
    time_t exp = std::time(nullptr) + TOKEN_TTL_SECONDS;
    std::string payload = user_id + "|" + std::to_string((long long)exp);
    std::string mac = hmac_sha256_hex(g_secret, payload);
    return payload + "." + mac;
}

// stateless — lock 불필요
static std::string verify_token(const std::string& token) {
    size_t dot = token.rfind('.');
    if (dot == std::string::npos) return "";
    std::string payload = token.substr(0, dot);
    std::string mac = token.substr(dot + 1);
    // 상수시간 비교 — 일반 != 비교는 byte-by-byte 누출로 시크릿 추측 가능
    if (!constant_time_eq(hmac_sha256_hex(g_secret, payload), mac)) return "";
    size_t bar = payload.find('|');
    if (bar == std::string::npos) return "";
    std::string user_id = payload.substr(0, bar);
    long long exp = std::atoll(payload.c_str() + bar + 1);
    if (std::time(nullptr) > exp) return "";
    return user_id;
}

// caller must hold g_state_mu
static void drain_queue() {
    evict_stale_nonces_locked();
    if (g_sold_out.load()) {
        if (!g_queue.empty()) {
            std::cout << "[대기열] 매진 — 대기열 " << g_queue.size() << "명 강제 종료\n";
            g_queue.clear();
        }
        return;
    }
    evict_stale();
    for (auto& e : g_queue) {
        if (!e.ready_token.empty()) continue;
        if (!g_bucket.try_consume()) break;
        e.ready_token = make_token(e.user_id);  // make_token 은 lock 무관
    }
}

static int eta_for_pos(int pos) {
    if (g_rate <= 0) return 9999;
    return int((pos + 1) / g_rate);
}

static std::string gen_nonce_hex() {
    // thread_local: 각 워커가 독립 RNG (락 불필요, 시드는 random_device)
    thread_local std::mt19937_64 rng(std::random_device{}());
    uint64_t v = rng();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx", (unsigned long)v);
    return std::string(buf);
}

// =============== Handlers ====================================================
static std::string handle_hello(const std::vector<std::string>& parts) {
    if (g_sold_out.load()) return "SOLD_OUT:all_taken";
    if (parts.size() < 2 || parts[1].empty()) return "AUTH_FAIL:bad_request";
    std::string device_id = parts[1];
    std::string nonce = gen_nonce_hex();

    std::lock_guard<std::mutex> lk(g_state_mu);
    g_nonces[device_id] = { nonce, std::time(nullptr) };
    g_stats.nonces_issued++;
    return "NONCE:" + nonce;
}

static std::string handle_auth(const std::vector<std::string>& parts) {
    if (g_sold_out.load()) return "SOLD_OUT:all_taken";
    if (parts.size() < 6) {
        std::lock_guard<std::mutex> lk(g_state_mu); g_stats.auth_fail++;
        return "AUTH_FAIL:bad_request";
    }
    const std::string& user_id     = parts[1];
    const std::string& device_id   = parts[2];
    const std::string& nonce       = parts[3];
    const std::string& sliding_log = parts[4];
    const std::string& client_hash = parts[5];

    if (user_id.empty() || device_id.empty()) {
        std::lock_guard<std::mutex> lk(g_state_mu); g_stats.auth_fail++;
        return "AUTH_FAIL:bad_request";
    }
    if (sliding_log.size() < MIN_SLIDING_LOG_LEN) {
        std::lock_guard<std::mutex> lk(g_state_mu); g_stats.auth_fail++;
        return "AUTH_FAIL:no_sliding";
    }

    // ★ CPU 집약 부분 — lock 밖에서 병렬 처리 (멀티스레드 효과의 핵심)
    std::string canonical = user_id + "|" + device_id + "|" + nonce + "|" + sliding_log;
    std::string expected = Sha256::hash_hex(canonical);

    // 상태 변경 구간: lock
    std::lock_guard<std::mutex> lk(g_state_mu);
    auto it = g_nonces.find(device_id);
    if (it == g_nonces.end())               { g_stats.auth_fail++; return "AUTH_FAIL:unknown_nonce"; }
    if (it->second.value != nonce)          { g_stats.auth_fail++; return "AUTH_FAIL:nonce_mismatch"; }
    if (std::time(nullptr) - it->second.issued_at > NONCE_TTL_SECONDS) {
        g_nonces.erase(it); g_stats.auth_fail++; return "AUTH_FAIL:nonce_expired";
    }
    if (!constant_time_eq(expected, client_hash)) { g_stats.auth_fail++; return "AUTH_FAIL:hash_mismatch"; }

    g_nonces.erase(it);

    drain_queue();
    if (g_queue.empty() && g_bucket.try_consume()) {
        g_stats.tokens_direct++;
        return "TOKEN:" + make_token(user_id);
    }

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
    if (g_sold_out.load()) return "SOLD_OUT:all_taken";
    if (parts.size() < 2 || parts[1].empty()) return "POLL_FAIL:bad_request";

    std::lock_guard<std::mutex> lk(g_state_mu);
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

// 내부 전용 — HMAC stateless 라서 검증 자체는 lock-free, 일회성 체크와 stats++ 만 lock
static std::string handle_verify(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        std::lock_guard<std::mutex> lk(g_state_mu); g_stats.verify_bad++;
        return "BAD_TOKEN";
    }
    std::string token = line.substr(colon + 1);
    std::string user = verify_token(token);   // lock 밖

    std::lock_guard<std::mutex> lk(g_state_mu);
    if (user.empty()) { g_stats.verify_bad++; return "BAD_TOKEN"; }

    // 일회성 enforcement — 한 토큰 = 한 좌석. 두 번째부터는 거부.
    // exp 파싱: payload = "<user>|<exp>", token = "<payload>.<mac>"
    size_t dot = token.rfind('.');
    size_t bar = token.find('|');
    if (dot != std::string::npos && bar != std::string::npos && bar < dot) {
        time_t exp = (time_t)std::atoll(token.c_str() + bar + 1);
        auto ins = g_used_tokens.emplace(token, exp);
        if (!ins.second) {
            g_stats.verify_reuse++;
            return "BAD_TOKEN";
        }
    }

    g_stats.verify_ok++;
    return "OK:" + user;
}

static std::string handle_notify_sold_out() {
    if (!g_sold_out.exchange(true)) {
        std::cout << "[대기열] 매진 통지 수신 — 이후 HELLO/AUTH/POLL 은 SOLD_OUT 응답\n";
    }
    return "OK";
}

// caller must hold g_state_mu
static std::string stats_line_locked() {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "active=%ld total_conn=%ld nonces=%ld tok_direct=%ld tok_queue=%ld "
        "queue=%ld peak_queue=%ld peak_active=%ld verify_ok=%ld verify_bad=%ld "
        "verify_reuse=%ld auth_fail=%ld threads=%d",
        g_stats.active_conn, g_stats.total_conn, g_stats.nonces_issued,
        g_stats.tokens_direct, g_stats.tokens_queued,
        (long)g_queue.size(), g_stats.peak_queue, g_stats.peak_active,
        g_stats.verify_ok, g_stats.verify_bad,
        g_stats.verify_reuse, g_stats.auth_fail, g_n_threads);
    return std::string(buf);
}

static void maybe_print_stats_locked() {
    auto now = clk::now();
    if (std::chrono::duration<double>(now - g_last_print).count() < 1.0) return;
    g_last_print = now;
    std::cout << "[stats] " << stats_line_locked() << std::endl;
}

static std::string dispatch(const std::string& line) {
    if (line.rfind("HELLO:", 0) == 0)  return handle_hello(split(line, ':'));
    if (line.rfind("AUTH:",  0) == 0)  return handle_auth(split(line, ':'));
    if (line.rfind("POLL:",  0) == 0)  return handle_poll(split(line, ':'));
    if (line.rfind("VERIFY:",0) == 0)  return handle_verify(line);
    if (line == "NOTIFY_SOLD_OUT")     return handle_notify_sold_out();
    if (line == "STATS") {
        std::lock_guard<std::mutex> lk(g_state_mu);
        return "STATS:" + stats_line_locked();
    }
    return "AUTH_FAIL:unknown_cmd";
}

// =============== Worker pool =================================================
struct Worker {
    int epfd = -1;
    std::thread th;
    std::unordered_map<int, std::string> client_buf;  // 워커 전용 (락 불필요)
};
static std::vector<Worker> g_workers;
static std::atomic<int> g_rr{0};  // round-robin counter

static void close_client(Worker& w, int fd) {
    ::epoll_ctl(w.epfd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    w.client_buf.erase(fd);
    std::lock_guard<std::mutex> lk(g_state_mu);
    g_stats.active_conn--;
}

static void worker_loop(int wid) {
    Worker& w = g_workers[wid];
    epoll_event events[128];
    while (!g_shutdown) {
        int n = ::epoll_wait(w.epfd, events, 128, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("worker epoll_wait");
            break;
        }
        if (n == 0) {
            std::lock_guard<std::mutex> lk(g_state_mu);
            drain_queue();
            continue;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            char buf[2048];
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r <= 0) { close_client(w, fd); continue; }

            std::string& acc = w.client_buf[fd];
            acc.append(buf, buf + r);
            if (acc.size() > MAX_LINE_BYTES) {
                // '\n' 없이 폭주하는 클라이언트 차단 — 메모리 무한 누적 방지
                close_client(w, fd);
                continue;
            }

            size_t pos;
            bool drop = false;
            while ((pos = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                std::string resp = dispatch(line);
                if (!send_line(fd, resp)) { drop = true; break; }
            }
            if (drop) close_client(w, fd);
        }

        // 매 wake 마다 drain (락 한 번)
        std::lock_guard<std::mutex> lk(g_state_mu);
        drain_queue();
    }
}

// =============================================================================

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    g_cap       = env_int("BUCKET_CAP",    BUCKET_CAPACITY);
    g_rate      = env_dbl("BUCKET_RATE",   BUCKET_REFILL_PER_SEC);
    g_queue_ttl = env_int("QUEUE_TTL",     QUEUE_TTL_SECONDS);
    g_n_threads = env_int("QUEUE_THREADS", 1);
    int g_port  = env_int("QUEUE_PORT",    QUEUE_SERVER_PORT);  // 듀얼 레인용
    if (g_n_threads < 1) g_n_threads = 1;
    if (g_n_threads > 64) g_n_threads = 64;
    const char* sec_env = std::getenv("DAEMON_SECRET");
    g_secret = (sec_env && *sec_env) ? std::string(sec_env) : std::string(DAEMON_SECRET);
    g_bucket.init();

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::perror("socket"); return 1; }
    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(g_port);

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    if (::listen(server_fd, SOMAXCONN) < 0) {
        std::perror("listen"); return 1;
    }

    // 워커 풀 기동
    g_workers.resize(g_n_threads);
    for (int i = 0; i < g_n_threads; i++) {
        g_workers[i].epfd = ::epoll_create1(0);
        if (g_workers[i].epfd < 0) { std::perror("epoll_create1"); return 1; }
        g_workers[i].th = std::thread(worker_loop, i);
    }

    // accept 전용 epoll (메인 스레드)
    int accept_epfd = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    ::epoll_ctl(accept_epfd, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "[대기열 서버] 127.0.0.1:" << g_port << " 가동"
              << " (threads=" << g_n_threads
              << " cap=" << g_cap
              << " rate=" << g_rate << "/s"
              << " queue_ttl=" << g_queue_ttl << "s)\n";

    epoll_event events[128];
    while (!g_shutdown) {
        int n = ::epoll_wait(accept_epfd, events, 128, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("accept epoll_wait");
            break;
        }
        if (n == 0) {
            std::lock_guard<std::mutex> lk(g_state_mu);
            maybe_print_stats_locked();
            continue;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd != server_fd) continue;  // 이 epoll 에는 listen socket 만 있음

            // accept 가능한 만큼 비빔 (burst 대응)
            while (true) {
                int cfd = ::accept4(server_fd, nullptr, nullptr, SOCK_NONBLOCK);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                int flag = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                // 라운드 로빈으로 워커에 등록
                int w = g_rr.fetch_add(1, std::memory_order_relaxed) % g_n_threads;
                epoll_event cev{};
                cev.events = EPOLLIN;
                cev.data.fd = cfd;
                if (::epoll_ctl(g_workers[w].epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                    ::close(cfd);
                    continue;
                }
                g_workers[w].client_buf[cfd] = "";
                std::lock_guard<std::mutex> lk(g_state_mu);
                g_stats.active_conn++;
                g_stats.total_conn++;
                if (g_stats.active_conn > g_stats.peak_active)
                    g_stats.peak_active = g_stats.active_conn;
            }
        }
        std::lock_guard<std::mutex> lk(g_state_mu);
        maybe_print_stats_locked();
    }

    std::cerr << "[대기열 서버] 종료 처리 중...\n";
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        std::cerr << "[대기열 서버] 최종 통계 " << stats_line_locked() << "\n";
    }
    // 워커 join
    for (auto& w : g_workers) {
        if (w.th.joinable()) w.th.join();
        if (w.epfd >= 0) ::close(w.epfd);
        for (auto& kv : w.client_buf) ::close(kv.first);
    }
    ::close(server_fd);
    ::close(accept_epfd);
    return 0;
}
