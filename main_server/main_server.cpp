// 메인 서버 (Main Server)
// - 데몬으로부터만 요청을 받음 (이미 검증된 요청)
// - mmap된 좌석 파일을 RAM처럼 사용
// - CAS(__atomic_compare_exchange_n) 으로 락프리 차감
// - epoll 비동기 처리

#include "../common/protocol.h"
#include "../common/netutil.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <iostream>

using namespace ticketing;

static int32_t* g_seats = nullptr;

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
    return fd;
}

static std::string handle_line(const std::string& line) {
    if (line.rfind("RESERVE:", 0) == 0) {
        int seat = std::atoi(line.c_str() + 8);
        if (seat < 0 || seat >= MAX_SEATS) return "RESULT:BAD_REQUEST";

        int32_t expected = 0;
        bool ok = __atomic_compare_exchange_n(
            &g_seats[seat], &expected, 1,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        if (ok) {
            std::cout << "[OK] CAS 통과 (좌석 " << seat << ")" << std::endl;
            return "RESULT:SUCCESS";
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
    while (true) {
        int n = ::epoll_wait(epfd, events, 128, -1);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == server_fd) {
                int cfd = ::accept(server_fd, nullptr, nullptr);
                if (cfd < 0) continue;
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
    return 0;
}
