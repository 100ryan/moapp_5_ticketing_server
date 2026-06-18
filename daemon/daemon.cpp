// 데몬 (라우터 / Gateway) — 듀얼 레인 큐 지원
// - 외부(앱)로부터 모든 요청을 받음
// - 명령에 따라 대기열 서버 또는 예매 서버로 라우팅
// - 상태/비밀키 없음 (둘 다 백엔드 서버가 보유)
//
// 환경변수:
//   LANE_PORTS="9092,9093"  → 듀얼/멀티 레인 큐. 기본은 단일 (QUEUE_SERVER_PORT)
//
// 라우팅:
//   HELLO/AUTH/POLL              → device_id 해시 % N → lane[k] 로 forward
//   STATS                        → 모든 lane 통계 합산
//   VERIFY (RESERVE 시)          → lane[0] (HMAC stateless, 모든 lane 동일)
//   NOTIFY_SOLD_OUT              → 모든 lane 에 broadcast
//   RESERVE:<token>:<seat>       → VERIFY → 예매 서버 RESERVE
//   FETCH_STATUS                 → 예매 서버 forward
//
// 프로토콜 (앱 ↔ 데몬):
//   HELLO:<device_id>                                            -> NONCE:<hex>
//   AUTH:<user_id>:<device_id>:<nonce>:<sliding_log>:<hash>      -> TOKEN | QUEUED | AUTH_FAIL | SOLD_OUT
//   POLL:<device_id>                                             -> TOKEN | QUEUED | POLL_FAIL | SOLD_OUT
//   RESERVE:<token>:<seat_idx>                                   -> RESULT:SUCCESS | FAIL_TAKEN | BAD_TOKEN | BAD_REQUEST
//   FETCH_STATUS                                                 -> STATUS:<MAX_SEATS 비트>
//   STATS                                                        -> STATS:k=v k=v ...

#include "../common/protocol.h"
#include "../common/netutil.h"

#include <sys/socket.h>
#include <sys/time.h>
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
#include <atomic>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <iostream>

using namespace ticketing;

static volatile sig_atomic_t g_shutdown = 0;
static void on_signal(int) { g_shutdown = 1; }

// 백엔드 연결 (영속, 한 번 맺고 재사용)
struct QueueLane {
    int port;
    int fd = -1;
    std::string recv_buf;
    explicit QueueLane(int p) : port(p) {}
};
static std::vector<QueueLane> g_lanes;
static int g_main_fd  = -1;
static std::string g_main_recv_buf;

// 매진 통지를 두 번 보내지 않기 위함
static std::atomic<bool> g_sold_out_notified{false};

// 백엔드(큐/메인 서버) 호출 타임아웃 — 백엔드 한 곳이 hang 되면 데몬의 단일 epoll 루프가
// 통째로 멈춰서 전 클라이언트가 같이 굳는 사고를 막기 위함. 로컬 호출이라 3초면 넉넉.
static void set_backend_timeouts(int fd) {
    struct timeval tv;
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// =============== Lane routing ================================================
// device_id 해시 → lane 인덱스. 같은 device 는 항상 같은 lane 으로 가야 큐 상태 일관성.
static int lane_for_device(const std::string& device_id) {
    if (g_lanes.size() <= 1) return 0;
    size_t h = std::hash<std::string>{}(device_id);
    return int(h % g_lanes.size());
}

// =============== Backend connection helpers ==================================
static bool ensure_lane_connection(int idx) {
    auto& L = g_lanes[idx];
    if (L.fd >= 0) return true;
    L.fd = connect_tcp("127.0.0.1", L.port);
    if (L.fd < 0) {
        std::cerr << "[데몬] 대기열 lane " << idx << " (port " << L.port << ") 연결 실패\n";
        return false;
    }
    set_backend_timeouts(L.fd);
    L.recv_buf.clear();
    std::cerr << "[데몬] 대기열 lane " << idx << " (port " << L.port << ") 연결 OK\n";
    return true;
}

static bool ensure_main_connection() {
    if (g_main_fd >= 0) return true;
    g_main_fd = connect_tcp("127.0.0.1", MAIN_SERVER_PORT);
    if (g_main_fd < 0) {
        std::cerr << "[데몬] 예매 서버 연결 실패\n";
        return false;
    }
    set_backend_timeouts(g_main_fd);
    g_main_recv_buf.clear();
    std::cerr << "[데몬] 예매 서버 연결 OK\n";
    return true;
}

// 한 줄 보내고 한 줄 받기 (블로킹). 실패 시 연결 닫고 false 같은 류의 응답.
static std::string lane_request(int idx, const std::string& req) {
    if (!ensure_lane_connection(idx)) return "AUTH_FAIL:queue_down";
    auto& L = g_lanes[idx];
    if (!send_line(L.fd, req)) {
        ::close(L.fd); L.fd = -1;
        return "AUTH_FAIL:queue_down";
    }
    while (true) {
        size_t pos = L.recv_buf.find('\n');
        if (pos != std::string::npos) {
            std::string line = L.recv_buf.substr(0, pos);
            L.recv_buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        char buf[2048];
        ssize_t r = ::read(L.fd, buf, sizeof(buf));
        if (r <= 0) {
            ::close(L.fd); L.fd = -1;
            return "AUTH_FAIL:queue_down";
        }
        L.recv_buf.append(buf, buf + r);
    }
}

// 모든 lane 에 broadcast (best-effort, 응답은 무시)
static void broadcast_lanes(const std::string& req) {
    for (size_t i = 0; i < g_lanes.size(); i++) {
        lane_request(int(i), req);
    }
}

// 모든 lane STATS 합산 (간단히 첫 번째 응답 반환 — 추후 합산 로직 추가 가능)
static std::string aggregate_stats() {
    if (g_lanes.empty()) return "STATS:no_lanes";
    if (g_lanes.size() == 1) return lane_request(0, "STATS");
    // 멀티 lane: 각각 받아서 lane=i 접두어 붙여 합침
    std::string out = "STATS:";
    for (size_t i = 0; i < g_lanes.size(); i++) {
        std::string r = lane_request(int(i), "STATS");
        if (r.rfind("STATS:", 0) == 0) r = r.substr(6);
        if (i > 0) out += " | ";
        out += "lane" + std::to_string(i) + "[" + r + "]";
    }
    return out;
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

// =============== RESERVE 처리 ================================================
// 클라이언트: RESERVE:<token>:<seat_idx>
// 1) 대기열 서버에 VERIFY:<token> → OK:<user> 또는 BAD_TOKEN
// 2) 통과 시 예매 서버에 RESERVE:<seat_idx> → RESULT:SUCCESS:<left> 등
// 3) 매진 감지(left == 0)되면 대기열 서버에 NOTIFY_SOLD_OUT
static std::string handle_reserve(const std::string& line) {
    size_t first  = line.find(':');
    size_t second = line.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos) return "RESULT:BAD_REQUEST";
    std::string token = line.substr(first + 1, second - first - 1);
    std::string seat_str = line.substr(second + 1);
    // 좌석 인덱스는 반드시 비어있지 않은 숫자 — 빈 문자열이 atoi("")=0 으로 좌석 0 예약되는 버그 방지
    if (seat_str.empty()) return "RESULT:BAD_REQUEST";
    for (char c : seat_str) if (c < '0' || c > '9') return "RESULT:BAD_REQUEST";
    int seat = std::atoi(seat_str.c_str());
    if (seat < 0 || seat >= MAX_SEATS) return "RESULT:BAD_REQUEST";

    // 1) 토큰 검증 (lane 0 — HMAC stateless 라 어느 lane 이든 동일 결과)
    std::string vresp = lane_request(0, "VERIFY:" + token);
    if (vresp.rfind("OK:", 0) != 0) {
        return "RESULT:BAD_TOKEN";
    }
    // (필요하면 vresp.substr(3) 로 user_id 추출 가능)

    // 2) 좌석 차감
    std::string r = main_request("RESERVE:" + std::to_string(seat));

    // 3) 응답 가공 + 매진 전파
    if (r.rfind("RESULT:SUCCESS:", 0) == 0) {
        int left = std::atoi(r.c_str() + 15);
        if (left <= 0 && !g_sold_out_notified.exchange(true)) {
            std::cout << "[데몬] 좌석 매진 감지 — 모든 대기열 lane 에 통지\n";
            broadcast_lanes("NOTIFY_SOLD_OUT");
        }
        return "RESULT:SUCCESS";
    }
    return r;  // RESULT:FAIL_TAKEN / RESULT:BAD_REQUEST 등 그대로
}

// 명령 별 device_id 추출 (lane 라우팅용)
//   HELLO:<dev>                                 → dev
//   AUTH:<user>:<dev>:<nonce>:<sliding>:<hash>  → dev (parts[2])
//   POLL:<dev>                                  → dev
static std::string extract_device_id(const std::string& line) {
    if (line.rfind("HELLO:", 0) == 0) return line.substr(6);
    if (line.rfind("POLL:",  0) == 0) return line.substr(5);
    if (line.rfind("AUTH:",  0) == 0) {
        size_t p1 = line.find(':');
        size_t p2 = line.find(':', p1 + 1);
        size_t p3 = line.find(':', p2 + 1);
        if (p2 != std::string::npos && p3 != std::string::npos)
            return line.substr(p2 + 1, p3 - p2 - 1);
    }
    return "";
}

// =============== 라우팅 ======================================================
static std::string dispatch(const std::string& line) {
    if (line.rfind("HELLO:", 0) == 0 ||
        line.rfind("AUTH:",  0) == 0 ||
        line.rfind("POLL:",  0) == 0) {
        std::string dev = extract_device_id(line);
        int lane = lane_for_device(dev);
        return lane_request(lane, line);
    }
    if (line.rfind("RESERVE:", 0) == 0) return handle_reserve(line);
    if (line == "FETCH_STATUS")         return main_request(line);
    if (line == "STATS")                return aggregate_stats();
    return "AUTH_FAIL:unknown_cmd";
}

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::perror("socket"); return 1; }
    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // 외부 노출
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

    // LANE_PORTS="9092,9093" 파싱 → g_lanes 채움. 없으면 기본 단일 lane.
    const char* lanes_env = std::getenv("LANE_PORTS");
    if (lanes_env && *lanes_env) {
        std::string s = lanes_env;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            int p = std::atoi(tok.c_str());
            if (p > 0) g_lanes.emplace_back(p);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (g_lanes.empty()) g_lanes.emplace_back(QUEUE_SERVER_PORT);

    // 시작 시 백엔드 연결 시도 (실패해도 첫 요청에서 다시 시도됨)
    for (size_t i = 0; i < g_lanes.size(); i++) ensure_lane_connection(int(i));
    ensure_main_connection();

    std::cout << "[데몬] 0.0.0.0:" << DAEMON_PORT << " 가동 (라우터, "
              << g_lanes.size() << "-lane queue)\n";
    for (size_t i = 0; i < g_lanes.size(); i++)
        std::cout << "       → 대기열 lane " << i << " : 127.0.0.1:" << g_lanes[i].port << "\n";
    std::cout << "       → 예매  서버      : 127.0.0.1:" << MAIN_SERVER_PORT << "\n";

    epoll_event events[128];
    while (!g_shutdown) {
        int n = ::epoll_wait(epfd, events, 128, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == server_fd) {
                int cfd = ::accept(server_fd, nullptr, nullptr);
                if (cfd < 0) continue;
                int flag = 1;
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                epoll_event cev{};
                cev.events = EPOLLIN;
                cev.data.fd = cfd;
                ::epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                client_buf[cfd] = "";
                continue;
            }

            char buf[2048];
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r <= 0) {
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                client_buf.erase(fd);
                continue;
            }
            std::string& acc = client_buf[fd];
            acc.append(buf, buf + r);
            if (acc.size() > MAX_LINE_BYTES) {
                // '\n' 없이 폭주하는 클라이언트 차단 — 메모리 무한 누적 방지
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                client_buf.erase(fd);
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
            if (drop) {
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                client_buf.erase(fd);
            }
        }
    }

    std::cerr << "[데몬] 종료 처리 중...\n";
    for (auto& kv : client_buf) ::close(kv.first);
    for (auto& L : g_lanes) if (L.fd >= 0) ::close(L.fd);
    if (g_main_fd >= 0) ::close(g_main_fd);
    ::close(server_fd);
    ::close(epfd);
    return 0;
}
