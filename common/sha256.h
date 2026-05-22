#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace ticketing {

class Sha256 {
public:
    Sha256();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s);
    void finalize(uint8_t out[32]);

    static std::string hex(const uint8_t* data, size_t len);
    static std::string hash_hex(const std::string& s);

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[8];
    uint64_t bitlen_;
    uint8_t buf_[64];
    size_t buflen_;
};

}
