CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread

COMMON_SRC = common/sha256.cpp common/hmac.cpp common/netutil.cpp

.PHONY: all clean init run-daemon run-main

all: daemon/daemon main_server/main_server tools/init_ticket_file

daemon/daemon: daemon/daemon.cpp $(COMMON_SRC) common/protocol.h common/sha256.h common/hmac.h common/netutil.h
	$(CXX) $(CXXFLAGS) -o $@ daemon/daemon.cpp $(COMMON_SRC)

main_server/main_server: main_server/main_server.cpp common/netutil.cpp common/protocol.h common/netutil.h
	$(CXX) $(CXXFLAGS) -o $@ main_server/main_server.cpp common/netutil.cpp

tools/init_ticket_file: tools/init_ticket_file.cpp common/protocol.h
	$(CXX) $(CXXFLAGS) -o $@ tools/init_ticket_file.cpp

init: tools/init_ticket_file
	./tools/init_ticket_file

run-main: main_server/main_server
	./main_server/main_server

run-daemon: daemon/daemon
	./daemon/daemon

clean:
	rm -f daemon/daemon main_server/main_server tools/init_ticket_file
