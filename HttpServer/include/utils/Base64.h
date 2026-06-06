#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace http {
namespace utils {

inline const std::array<int8_t, 256>& base64DecodeTable()
{
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(chars[i])] = static_cast<int8_t>(i);
        return t;
    }();
    return table;
}

inline std::vector<uint8_t> base64Decode(std::string_view encoded)
{
    if (encoded.rfind("data:", 0) == 0) {
        size_t comma = encoded.find(',');
        if (comma == std::string_view::npos)
            return {};
        encoded.remove_prefix(comma + 1);
    }

    const auto& table = base64DecodeTable();
    std::vector<uint8_t> result;
    result.reserve((encoded.size() * 3) / 4);

    int val = 0;
    int valb = -8;
    for (unsigned char c : encoded)
    {
        if (c == '=')
            break;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ')
            continue;

        int decoded = table[c];
        if (decoded < 0)
            return {};

        val = (val << 6) | decoded;
        valb += 6;
        if (valb >= 0)
        {
            result.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

} // namespace utils
} // namespace http
