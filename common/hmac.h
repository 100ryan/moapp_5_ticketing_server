#pragma once

#include <string>

namespace ticketing {

std::string hmac_sha256_hex(const std::string& key, const std::string& msg);

}
