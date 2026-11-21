#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace syntwine {

inline std::optional<std::size_t>
utf8_characters(std::string_view input) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < input.size(); ++count) {
        const auto first = static_cast<unsigned char>(input[i++]);
        if (first < 0x80)
            continue;
        if (first < 0xc2 || first > 0xf4)
            return std::nullopt;
        const unsigned tail = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        if (input.size() - i < tail)
            return std::nullopt;
        const auto second = static_cast<unsigned char>(input[i]);
        if ((first == 0xe0 && second < 0xa0)
            || (first == 0xed && second >= 0xa0)
            || (first == 0xf0 && second < 0x90)
            || (first == 0xf4 && second >= 0x90))
            return std::nullopt;
        for (unsigned j = 0; j < tail; ++j)
            if ((static_cast<unsigned char>(input[i++]) & 0xc0) != 0x80)
                return std::nullopt;
    }
    return count;
}

} // namespace syntwine
