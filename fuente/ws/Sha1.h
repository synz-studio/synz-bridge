// SHA-1 (RFC 3174). Solo se usa para el handshake de WebSocket (RFC 6455), donde el estándar lo exige;
// no sirve ni se usa para nada de seguridad.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace synz
{
inline std::array<uint8_t, 20> sha1 (const void* datos, size_t n)
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    auto rotar = [] (uint32_t x, int k) { return (x << k) | (x >> (32 - k)); };

    auto bloque = [&] (const uint8_t* b)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t) b[4 * i] << 24 | (uint32_t) b[4 * i + 1] << 16 | (uint32_t) b[4 * i + 2] << 8 | (uint32_t) b[4 * i + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = rotar (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b2 = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b2 & c) | (~b2 & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b2 ^ c ^ d;                      k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b2 & c) | (b2 & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b2 ^ c ^ d;                      k = 0xCA62C1D6u; }
            const uint32_t t = rotar (a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotar (b2, 30); b2 = a; a = t;
        }
        h[0] += a; h[1] += b2; h[2] += c; h[3] += d; h[4] += e;
    };

    const auto* p = static_cast<const uint8_t*> (datos);
    size_t resto = n;
    while (resto >= 64) { bloque (p); p += 64; resto -= 64; }

    // Relleno: 0x80, ceros y la longitud en bits (64 bits, big-endian).
    uint8_t fin[128] = {};
    std::memcpy (fin, p, resto);
    fin[resto] = 0x80;
    const size_t total = (resto < 56) ? 64 : 128;
    const uint64_t bits = (uint64_t) n * 8u;
    for (int i = 0; i < 8; ++i)
        fin[total - 1 - (size_t) i] = (uint8_t) (bits >> (8 * i));
    bloque (fin);
    if (total == 128) bloque (fin + 64);

    std::array<uint8_t, 20> r {};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            r[(size_t) (4 * i + j)] = (uint8_t) (h[i] >> (24 - 8 * j));
    return r;
}
} // namespace synz
