#pragma once

#include <string>
#include <vector>

namespace ticketing {

bool send_line(int fd, const std::string& s);

std::vector<std::string> split(const std::string& s, char delim);

int connect_tcp(const char* host, int port);

}
