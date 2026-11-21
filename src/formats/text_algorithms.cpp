#include "syntwine/detail/formats/text_algorithms.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <unordered_set>

namespace syntwine::strings {
namespace {

bool continuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xc0U) == 0x80U;
}

std::size_t next_scalar(std::string_view text, std::size_t pos) noexcept {
    while (++pos < text.size() && continuation(text[pos])) {}
    return pos;
}

std::size_t previous_scalar(std::string_view text, std::size_t pos) noexcept {
    while (continuation(text[--pos])) {}
    return pos;
}

char32_t scalar(std::string_view text, std::size_t pos) noexcept {
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80U)
        return lead;
    const unsigned length = lead < 0xe0U ? 2U : lead < 0xf0U ? 3U : 4U;
    char32_t value = lead & (0x7fU >> length);
    for (unsigned i = 1; i < length; ++i)
        value = (value << 6U) | (static_cast<unsigned char>(text[pos + i]) & 0x3fU);
    return value;
}

bool ascii_space(unsigned char value) noexcept {
    return value == ' ' || (value >= '\t' && value <= '\r');
}

bool unicode_space(char32_t value) noexcept {
    if (value < 0x80)
        return ascii_space(static_cast<unsigned char>(value)) || (value >= 0x1c && value <= 0x1f);
    return (value >= 0x2000 && value <= 0x200a) || value == 0x85 || value == 0xa0 ||
           value == 0x1680 || value == 0x2028 || value == 0x2029 || value == 0x202f ||
           value == 0x205f || value == 0x3000;
}

struct Line {
    std::string_view body;
    std::string_view newline;
};

std::size_t line_ending(std::string_view tail, bool utf8) noexcept {
    const auto first = static_cast<unsigned char>(tail.front());
    if (first == '\r')
        return tail.starts_with("\r\n") ? 2U : 1U;
    if (first == '\n')
        return 1;
    if (!utf8)
        return 0;
    if (first == '\v' || first == '\f' || (first >= 0x1c && first <= 0x1e))
        return 1;
    if (first == 0xc2 && tail.starts_with("\xc2\x85"))
        return 2;
    if (first == 0xe2 && (tail.starts_with("\xe2\x80\xa8") || tail.starts_with("\xe2\x80\xa9")))
        return 3;
    return 0;
}

Line next_line(std::string_view& remaining, bool utf8 = false) {
    for (std::size_t pos = 0; pos < remaining.size(); ++pos) {
        if (const auto size = line_ending(remaining.substr(pos), utf8)) {
            auto line = Line{remaining.substr(0, pos), remaining.substr(pos, size)};
            remaining.remove_prefix(pos + size);
            return line;
        }
    }
    auto line = Line{remaining, {}};
    remaining = {};
    return line;
}

constexpr std::array<std::uint32_t, 64> sha_keys{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

using ShaState = std::array<std::uint32_t, 8>;

void sha_block(ShaState& state, std::string_view block) {
    std::array<std::uint32_t, 64> words;
    for (std::size_t i = 0; i < 16; ++i) {
        const auto* byte = reinterpret_cast<const unsigned char*>(block.data() + i * 4);
        words[i] = (std::uint32_t{byte[0]} << 24U) | (std::uint32_t{byte[1]} << 16U) |
                   (std::uint32_t{byte[2]} << 8U) | std::uint32_t{byte[3]};
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15];
        const auto y = words[i - 2];
        words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U)) +
                   words[i - 7] + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10U));
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                           ((e & f) ^ (~e & g)) + sha_keys[i] + words[i];
        const auto second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
                            ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + first;
        d = c; c = b; b = a; a = first + second;
    }
    const ShaState result{a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < state.size(); ++i)
        state[i] += result[i];
}
} // namespace

std::size_t count(std::string_view text) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char byte) {
        return !continuation(byte);
    }));
}

std::string slice(std::string_view text, bool utf8, std::ptrdiff_t start,
                  std::ptrdiff_t step, std::size_t length) {
    if (length == 0)
        return {};
    if (!utf8) {
        if (step == 1)
            return std::string{text.substr(static_cast<std::size_t>(start), length)};
        std::string result(length, '\0');
        for (std::size_t i = 0; i < length; ++i) {
            result[i] = text[static_cast<std::size_t>(start)];
            if (i + 1 < length)
                start += step;
        }
        return result;
    }
    std::size_t pos = 0;
    for (std::ptrdiff_t i = 0; i < start; ++i)
        pos = next_scalar(text, pos);
    if (step == 1) {
        auto end = pos;
        for (std::size_t i = 0; i < length; ++i)
            end = next_scalar(text, end);
        return std::string{text.substr(pos, end - pos)};
    }
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        auto end = next_scalar(text, pos);
        result.append(text.substr(pos, end - pos));
        if (i + 1 == length)
            break;
        for (std::ptrdiff_t j = 0; j < step; ++j)
            pos = next_scalar(text, pos);
        for (std::ptrdiff_t j = 0; j > step; --j)
            pos = previous_scalar(text, pos);
    }
    return result;
}

std::string_view strip(std::string_view text, Side side, bool utf8,
                       const std::string_view* chars) {
    std::unordered_set<char32_t> trim;
    if (chars)
        for (std::size_t pos = 0; pos < chars->size(); pos = next_scalar(*chars, pos))
            trim.insert(scalar(*chars, pos));
    auto matches = [&](std::size_t pos) {
        if (!utf8)
            return ascii_space(static_cast<unsigned char>(text[pos]));
        const auto value = scalar(text, pos);
        return chars ? trim.contains(value) : unicode_space(value);
    };

    std::size_t begin = 0, end = text.size();
    if (side != Side::right)
        while (begin < end && matches(begin))
            begin = utf8 ? next_scalar(text, begin) : begin + 1;
    if (side != Side::left) {
        while (begin < end) {
            const auto prev = utf8 ? previous_scalar(text, end) : end - 1;
            if (!matches(prev))
                break;
            end = prev;
        }
    }
    return text.substr(begin, end - begin);
}

std::string replace(std::string_view text, std::string_view old,
                    std::string_view replacement, std::ptrdiff_t limit, bool utf8) {
    if (limit == 0 || old == replacement)
        return std::string{text};
    std::string result;
    result.reserve(text.size());
    std::size_t pos = 0;
    if (old.empty()) {
        while (limit != 0) {
            result.append(replacement);
            if (pos == text.size())
                return result;
            const auto next = utf8 ? next_scalar(text, pos) : pos + 1;
            result.append(text.substr(pos, next - pos));
            pos = next;
            if (limit > 0)
                --limit;
        }
        result.append(text.substr(pos));
        return result;
    }
    while (limit != 0) {
        const auto match = text.find(old, pos);
        if (match == std::string_view::npos)
            break;
        result.append(text.substr(pos, match - pos));
        result.append(replacement);
        pos = match + old.size();
        if (limit > 0)
            --limit;
    }
    result.append(text.substr(pos));
    return result;
}

std::vector<std::string> split(std::string_view text, const std::string_view* delimiter,
                               std::ptrdiff_t limit) {
    std::vector<std::string> result;
    if (delimiter) {
        if (delimiter->empty())
            throw std::invalid_argument("empty separator");
        std::size_t pos = 0;
        while (limit != 0) {
            const auto end = text.find(*delimiter, pos);
            if (end == std::string_view::npos)
                break;
            result.emplace_back(text.substr(pos, end - pos));
            pos = end + delimiter->size();
            if (limit > 0)
                --limit;
        }
        result.emplace_back(text.substr(pos));
        return result;
    }

    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && unicode_space(scalar(text, pos)))
            pos = next_scalar(text, pos);
        if (pos == text.size())
            break;
        if (limit == 0) {
            result.emplace_back(text.substr(pos));
            break;
        }
        auto end = pos;
        while (end < text.size() && !unicode_space(scalar(text, end)))
            end = next_scalar(text, end);
        result.emplace_back(text.substr(pos, end - pos));
        pos = end;
        if (limit > 0)
            --limit;
    }
    return result;
}

std::string indent(std::string_view text, std::size_t spaces, bool utf8) {
    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
        const auto [body, newline] = next_line(text, utf8);
        if (!strip(body, Side::left, utf8).empty())
            result.append(spaces, ' ');
        result.append(body).append(newline);
    }
    return result;
}

std::string dedent(std::string_view text, bool utf8) {
    std::string_view margin;
    bool first = true;
    auto remaining = text;
    while (!remaining.empty()) {
        const auto [body, newline] = next_line(remaining, utf8);
        const auto pos = body.find_first_not_of(" \t");
        if (pos == std::string_view::npos)
            continue;
        const auto prefix = body.substr(0, pos);
        if (first) {
            margin = prefix;
            first = false;
            continue;
        }
        const auto [end, other] = std::ranges::mismatch(margin, prefix);
        margin = margin.substr(0, static_cast<std::size_t>(end - margin.begin()));
    }

    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
        const auto [body, newline] = next_line(text, utf8);
        if (body.find_first_not_of(" \t") != std::string_view::npos)
            result.append(body.substr(margin.size()));
        result.append(newline);
    }
    return result;
}

std::string_view newline_for(std::string_view style) {
    if (style == "lf" || style == "\n")
        return "\n";
    if (style == "crlf" || style == "\r\n")
        return "\r\n";
    if (style == "cr" || style == "\r")
        return "\r";
    throw std::invalid_argument("unknown newline style");
}

std::string normalize_newlines(std::string_view text, std::string_view newline) {
    std::string result;
    result.reserve(text.size());
    while (!text.empty()) {
        const auto [body, old] = next_line(text);
        result.append(body);
        if (!old.empty())
            result.append(newline);
    }
    return result;
}

std::string squish(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    std::string_view after, blank;
    while (!text.empty()) {
        const auto [raw, newline] = next_line(text, true);
        const auto body = strip(raw, Side::both, true);
        if (body.empty()) {
            if (blank.empty())
                blank = newline;
            continue;
        }
        if (!result.empty())
            result.append(after).append(blank);
        bool space = false;
        for (std::size_t pos = 0; pos < body.size();) {
            const auto next = next_scalar(body, pos);
            if (unicode_space(scalar(body, pos))) {
                space = true;
            } else {
                if (space)
                    result.push_back(' ');
                space = false;
                result.append(body.substr(pos, next - pos));
            }
            pos = next;
        }
        after = newline;
        blank = {};
    }
    return result.append(after);
}

std::string base64(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(bytes.size() / 3 * 4 + (bytes.size() % 3 == 0 ? 0 : 4));
    for (std::size_t i = 0; i < bytes.size();) {
        const auto left = bytes.size() - i;
        const unsigned a = static_cast<unsigned char>(bytes[i++]);
        const unsigned b = left >= 2 ? static_cast<unsigned char>(bytes[i++]) : 0;
        const unsigned c = left >= 3 ? static_cast<unsigned char>(bytes[i++]) : 0;
        output.push_back(alphabet[a >> 2U]);
        output.push_back(alphabet[((a & 3U) << 4U) | (b >> 4U)]);
        output.push_back(left >= 2 ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=');
        output.push_back(left >= 3 ? alphabet[c & 63U] : '=');
    }
    return output;
}

std::string hex(std::string_view bytes) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        output.push_back(alphabet[byte >> 4U]);
        output.push_back(alphabet[byte & 15U]);
    }
    return output;
}

std::string sha256(std::string_view bytes) {
    ShaState state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto remaining = bytes;
    while (remaining.size() >= 64) {
        sha_block(state, remaining.substr(0, 64));
        remaining.remove_prefix(64);
    }

    // FIPS 180-4 padding: one bit, zeros, and a 64-bit big-endian bit length.
    const auto size = remaining.size() < 56 ? 64U : 128U;
    std::array<char, 128> padding{};
    std::ranges::copy(remaining, padding.begin());
    padding[remaining.size()] = static_cast<char>(0x80);
    const auto length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    for (unsigned i = 0; i < 8; ++i)
        padding[size - i - 1] = static_cast<char>(length >> (i * 8U));
    sha_block(state, {padding.data(), 64});
    if (size == 128)
        sha_block(state, {padding.data() + 64, 64});

    std::array<char, 32> digest;
    for (std::size_t i = 0; i < state.size(); ++i)
        for (unsigned j = 0; j < 4; ++j)
            digest[4 * i + j] = static_cast<char>(state[i] >> ((3U - j) * 8U));
    return hex({digest.data(), digest.size()});
}

} // namespace syntwine::strings
