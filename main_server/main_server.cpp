// 메인 서버 (Main Server)
// - 데몬으로부터만 요청을 받음 (이미 검증된 요청)
// - mmap된 좌석 파일을 RAM처럼 사용
// - CAS(__atomic_compare_exchange_n) 으로 락프리 차감
// - epoll 비동기 처리

#include "../common/protocol.h"
#include "../common/netutil.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>
#include <unordered_map>
#include <iostream>

using namespace ticketing;

static int32_t* g_seats = nullptr;
static std::atomic<int> g_seats_left{0};   // 남은 빈 좌석 수 (0 되면 매진)

// gprof 는 정상 종료(return from main / exit) 시에만 gmon.out 을 작성한다.
// 무한 epoll 루프를 SIGTERM/SIGINT 로 빠져나오기 위한 플래그.
static volatile sig_atomic_t g_shutdown = 0;
static void on_signal(int) { g_shutdown = 1; }

static int open_and_map_seats() {
    int fd = ::open(TICKET_FILE, O_RDWR);
    if (fd < 0) {
        std::perror("open seats.bin (tools/init_ticket_file 으로 먼저 초기화하세요)");
        return -1;
    }
    size_t size = sizeof(int32_t) * MAX_SEATS;
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap");
        ::close(fd);
        return -1;
    }
    g_seats = static_cast<int32_t*>(p);

    // 초기 빈 좌석 수 계산
    int left = 0;
    for (int i = 0; i < MAX_SEATS; i++) if (g_seats[i] == 0) left++;
    g_seats_left.store(left);
    std::cout << "[메인 서버] 초기 빈 좌석 " << left << " / " << MAX_SEATS << "\n";
    if (left == 0) {
        // 시작부터 매진이면 클라이언트가 HELLO→TOKEN까지 다 받고 RESERVE에서야 FAIL_TAKEN.
        // 큐 서버에는 단방향이라 알릴 채널이 없어서, 운영자가 알아챌 수 있도록 STDERR 경고.
        std::cerr << "[메인 서버] 경고: 시작 시 모든 좌석이 이미 매진 — seats.bin 재초기화 필요\n";
    }

    return fd;
}

static std::string handle_line(const std::string& line) {
    if (line.rfind("RESERVE:", 0) == 0) {
        // 좌석 인덱스는 반드시 비어있지 않은 숫자 — 빈 문자열이 atoi("")=0 으로 좌석 0 예약되는 버그 방지
        const char* sp = line.c_str() + 8;
        if (*sp == '\0') return "RESULT:BAD_REQUEST";
        for (const char* q = sp; *q; q++) if (*q < '0' || *q > '9') return "RESULT:BAD_REQUEST";
        int seat = std::atoi(sp);
        if (seat < 0 || seat >= MAX_SEATS) return "RESULT:BAD_REQUEST";

        int32_t expected = 0;
        bool ok = __atomic_compare_exchange_n(
            &g_seats[seat], &expected, 1,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        if (ok) {
            int left = g_seats_left.fetch_sub(1) - 1;
            std::cout << "[OK] CAS 통과 (좌석 " << seat << ", 남은 " << left << ")" << std::endl;
            // 응답 형식: RESULT:SUCCESS:<남은좌석수>
            return "RESULT:SUCCESS:" + std::to_string(left);
        } else {
            return "RESULT:FAIL_TAKEN";
        }
    }
    if (line == "FETCH_STATUS") {
        std::string s = "STATUS:";
        s.reserve(7 + MAX_SEATS);
        for (int i = 0; i < MAX_SEATS; i++) {
            int v = __atomic_load_n(&g_seats[i], __ATOMIC_RELAXED);
            s += (v ? '1' : '0');
        }
        return s;
    }
    return "RESULT:BAD_REQUEST";
}

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // send() 가 죽은 연결로 가도 프로세스 안 죽게

    int seats_fd = open_and_map_seats();
    if (seats_fd < 0) return 1;

    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::perror("socket"); return 1; }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(MAIN_SERVER_PORT);

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

    std::unordered_map<int, std::string> recv_buf;

    std::cout << "[메인 서버] 127.0.0.1:" << MAIN_SERVER_PORT
              << " 가동, " << MAX_SEATS << "석 mmap + CAS\n";

    epoll_event events[128];
    while (!g_shutdown) {
        int n = ::epoll_wait(epfd, events, 128, -1);
        if (n < 0) {
            if (errno == EINTR) continue;  // 시그널로 깬 것 — 다음 while 검사에서 빠짐
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
                recv_buf[cfd] = "";
                continue;
            }

            char buf[2048];
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r <= 0) {
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                recv_buf.erase(fd);
                continue;
            }
            std::string& acc = recv_buf[fd];
            acc.append(buf, buf + r);
            if (acc.size() > MAX_LINE_BYTES) {
                // '\n' 없이 폭주하는 연결 차단 — 메모리 무한 누적 방지
                ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                recv_buf.erase(fd);
                continue;
            }

            size_t pos;
            while ((pos = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                std::string resp = handle_line(line);
                if (!send_line(fd, resp)) {
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    ::close(fd);
                    recv_buf.erase(fd);
                    break;
                }
            }
        }
    }

    std::cerr << "[메인 서버] 종료 처리 중... (gmon.out 작성)\n";
    for (auto& kv : recv_buf) ::close(kv.first);
    ::close(server_fd);
    ::close(epfd);
    if (g_seats) ::munmap(g_seats, sizeof(int32_t) * MAX_SEATS);
    ::close(seats_fd);
    return 0;
}
