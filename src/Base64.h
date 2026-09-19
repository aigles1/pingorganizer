#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal base64, used only to carry raw pty bytes across the JS bridge as a
// JSON-safe string.
namespace base64
{
    inline std::string Encode(const char* data, size_t len)
    {
        static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        out.reserve(((len + 2) / 3) * 4);

        size_t i = 0;
        for (; i + 2 < len; i += 3)
        {
            const uint32_t n = (uint8_t)data[i] << 16 | (uint8_t)data[i + 1] << 8 | (uint8_t)data[i + 2];
            out += T[(n >> 18) & 63];
            out += T[(n >> 12) & 63];
            out += T[(n >> 6) & 63];
            out += T[n & 63];
        }

        if (i < len)
        {
            const bool two = (i + 1 < len);
            const uint32_t n = (uint8_t)data[i] << 16 | (two ? (uint8_t)data[i + 1] << 8 : 0);
            out += T[(n >> 18) & 63];
            out += T[(n >> 12) & 63];
            out += two ? T[(n >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    inline std::vector<char> Decode(const std::string& in)
    {
        static int8_t rev[256];
        static const bool init = []
        {
            for (int i = 0; i < 256; i++)
                rev[i] = -1;
            const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 64; i++)
                rev[(uint8_t)T[i]] = (int8_t)i;
            return true;
        }();
        (void)init;

        std::vector<char> out;
        out.reserve(in.size() / 4 * 3);

        uint32_t acc = 0;
        int bits = 0;
        for (const char ch : in)
        {
            const int8_t v = rev[(uint8_t)ch];
            if (v < 0)
                continue;                       // '=' and any stray whitespace
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back((char)((acc >> bits) & 0xFF));
            }
        }
        return out;
    }
}
