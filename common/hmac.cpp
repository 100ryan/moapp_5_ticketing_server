#include "hmac.h"
#include "sha256.h"
#include <cstdint>
#include <cstring>

namespace ticketing {

constexpr size_t BLOCK = 64;

std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    uint8_t k[BLOCK] = {0};

    if (key.size() > BLOCK) {
        Sha256 h;
        h.update(key);
        h.finalize(k);
    } else {
        std::memcpy(k, key.data(), key.size());
    }

    uint8_t ipad[BLOCK], opad[BLOCK];
    for (size_t i = 0; i < BLOCK; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner[32];
    {
        Sha256 h;
        h.update(ipad, BLOCK);
        h.update(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        h.finalize(inner);
    }

    uint8_t outer[32];
    {
        Sha256 h;
        h.update(opad, BLOCK);
        h.update(inner, 32);
        h.finalize(outer);
    }

    return Sha256::hex(outer, 32);
}

bool constant_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

}
