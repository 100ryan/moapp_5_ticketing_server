CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread

COMMON_SRC = common/sha256.cpp common/hmac.cpp common/netutil.cpp

.PHONY: all clean init run-daemon run-main run-queue profile

all: daemon/daemon main_server/main_server queue_server/queue_server tools/init_ticket_file

daemon/daemon: daemon/daemon.cpp common/netutil.cpp common/protocol.h common/netutil.h
	$(CXX) $(CXXFLAGS) -o $@ daemon/daemon.cpp common/netutil.cpp

queue_server/queue_server: queue_server/queue_server.cpp $(COMMON_SRC) common/protocol.h common/sha256.h common/hmac.h common/netutil.h
	$(CXX) $(CXXFLAGS) -o $@ queue_server/queue_server.cpp $(COMMON_SRC)

main_server/main_server: main_server/main_server.cpp common/netutil.cpp common/protocol.h common/netutil.h
	$(CXX) $(CXXFLAGS) -o $@ main_server/main_server.cpp common/netutil.cpp

tools/init_ticket_file: tools/init_ticket_file.cpp common/protocol.h
	$(CXX) $(CXXFLAGS) -o $@ tools/init_ticket_file.cpp

init: tools/init_ticket_file
	./tools/init_ticket_file

run-main: main_server/main_server
	./main_server/main_server

run-queue: queue_server/queue_server
	./queue_server/queue_server

run-daemon: daemon/daemon
	./daemon/daemon

# 측정용 빌드: -pg (gprof), -O0 (inlining 끔, 함수 attribution 정확), -g (소스 라인)
# 사용:
#   make profile
#   bash tools/profile.sh         # 자동 부하 + gprof + strace
profile:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="-std=c++17 -Wall -Wextra -pthread -pg -O0 -g" all
	@echo ""
	@echo "[profile] -pg 빌드 완료. tools/profile.sh 로 자동 측정하세요."

clean:
	rm -f daemon/daemon main_server/main_server queue_server/queue_server tools/init_ticket_file
	rm -f gmon.out gmon.main.* gmon.daemon.* gmon.queue.*
	rm -rf main_server/.prof daemon/.prof queue_server/.prof
