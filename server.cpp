#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <atomic>
#include <ctime>
#include <cstdlib>

#define PORT 8080
#define MAX_SEATS 200

// 200석 규모의 락-프리 좌석 배열
std::atomic<int> seats[MAX_SEATS];

int main() {
    srand(time(NULL));

    // 초기화: 일부 좌석(7의 배수)은 이미 팔린 상태(1)로 세팅하여 스타디움 화면에 회색으로 보이게 함
    for(int i = 0; i < MAX_SEATS; i++) {
        if (i % 7 == 0) seats[i] = 1; 
        else seats[i] = 0;
    }

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) exit(EXIT_FAILURE);
    
    // 포트 튕김 방지 마법의 코드
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, SOMAXCONN);

    // Epoll 대규모 트래픽 엔진 세팅
    int epoll_fd = epoll_create1(0);
    struct epoll_event event, events[100];
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    std::cout << "======================================" << std::endl;
    std::cout << "[티켓팅 코어] 200석 규모 스타디움 엔진 가동..." << std::endl;
    std::cout << "-> Epoll 멀티플렉싱 & CAS 락프리 활성화" << std::endl;
    std::cout << "======================================" << std::endl;

    while (true) {
        int event_count = epoll_wait(epoll_fd, events, 100, -1);
        for (int i = 0; i < event_count; i++) {
            if (events[i].data.fd == server_fd) {
                int client_fd = accept(server_fd, NULL, NULL);
                event.events = EPOLLIN;
                event.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event);
            } else {
                int client_fd = events[i].data.fd;
                char buffer[1024] = {0};
                int bytes_read = read(client_fd, buffer, 1024);
                
                if (bytes_read <= 0) {
                    close(client_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    continue;
                }

                std::string request(buffer);
                std::string response = "";

                // 1. 좌석 상태 가져오기 요청
                if (request.find("FETCH_STATUS") != std::string::npos) {
                    for(int j = 0; j < MAX_SEATS; j++) response += std::to_string(seats[j]);
                    response += "\n";
                }
                // 2. 생체 인증 및 토큰 발급 요청 (아까 빠져서 매크로로 인식되던 원인!)
                else if (request.find("SECURE_PAYLOAD") != std::string::npos) {
                    time_t now = time(0);
                    int random_num = rand() % 9000 + 1000;
                    std::string dynamic_token = "HMAC_" + std::to_string(now) + "_" + std::to_string(random_num);
                    response = "TOKEN:" + dynamic_token + "\n";
                    std::cout << "-> [정상] 일회성 인증 토큰 발급: " << dynamic_token << std::endl;
                }
                // 3. CAS 기반 최종 결제 요청
                else if (request.find("RESERVE:") != std::string::npos) {
                    int first_colon = request.find(":");
                    int second_colon = request.find(":", first_colon + 1);
                    if (second_colon != std::string::npos) {
                        int seat_idx = std::stoi(request.substr(first_colon + 1, second_colon - first_colon - 1));
                        int expected = 0;
                        
                        // CAS 원자적 락-프리 연산
                        if (seats[seat_idx].compare_exchange_strong(expected, 1)) {
                            response = "RESULT:SUCCESS\n";
                            std::cout << "✅ [예매 성공] CAS 연산 통과 (좌석 " << seat_idx << ")" << std::endl;
                        } else {
                            response = "RESULT:FAIL_TAKEN\n";
                            std::cout << "❌ [이선좌] CAS 연산 충돌 차단 (좌석 " << seat_idx << ")" << std::endl;
                        }
                    }
                }

                if(!response.empty()) {
                    send(client_fd, response.c_str(), response.length(), 0);
                }
                close(client_fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
            }
        }
    }
    return 0;
}