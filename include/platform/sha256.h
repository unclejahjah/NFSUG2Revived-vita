#ifndef PLATFORM_SHA256_H_
#define PLATFORM_SHA256_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace pf
{

// SHA-256 of a buffer as 64 lower-case hex digits.
std::string sha256Hex(const uint8_t* data, size_t size);

// SHA-256 fed in pieces.
class Sha256
{
public:
    void update(const uint8_t* data, size_t size);
    std::string hex();          // ends the hash

private:
    uint32_t m_h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t m_block[64] = {};
    size_t m_used = 0;
    uint64_t m_total = 0;
};

}

#endif
