// 좌석 데이터 파일 초기화 도구
// data/seats.bin 에 int32_t[MAX_SEATS] 길이의 binary 파일을 만든다.
// 7의 배수 인덱스는 이미 팔린(1) 상태로 세팅 (기존 server.cpp 동작 유지)

#include "../common/protocol.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>

int main() {
    mkdir("data", 0755);

    FILE* f = std::fopen(ticketing::TICKET_FILE, "wb");
    if (!f) {
        std::perror("fopen");
        return 1;
    }

    int32_t seats[ticketing::MAX_SEATS];
    for (int i = 0; i < ticketing::MAX_SEATS; i++) {
        seats[i] = (i % 7 == 0) ? 1 : 0;
    }

    if (std::fwrite(seats, sizeof(int32_t), ticketing::MAX_SEATS, f) != ticketing::MAX_SEATS) {
        std::perror("fwrite");
        std::fclose(f);
        return 1;
    }
    std::fclose(f);

    std::printf("초기화 완료: %s (%d석, %zu 바이트)\n",
                ticketing::TICKET_FILE, ticketing::MAX_SEATS,
                sizeof(int32_t) * ticketing::MAX_SEATS);
    return 0;
}
