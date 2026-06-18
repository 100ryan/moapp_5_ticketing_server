#pragma once

#include <string>

namespace ticketing {

std::string hmac_sha256_hex(const std::string& key, const std::string& msg);

// 두 문자열이 같은지 검사하되 길이가 같을 때는 항상 모든 바이트를 비교 (early-return 없음).
// HMAC/해시 비교에서 byte-by-byte timing leak 으로 시크릿을 한 글자씩 맞춰가는 공격 차단.
bool constant_time_eq(const std::string& a, const std::string& b);

}
