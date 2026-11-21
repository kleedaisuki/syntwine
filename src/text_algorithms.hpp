#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace syntwine::strings {

enum class Side { left, right, both };

std::size_t count(std::string_view text) noexcept;
std::string slice(std::string_view text, bool utf8, std::ptrdiff_t start,
                  std::ptrdiff_t step, std::size_t length);
std::string_view strip(std::string_view text, Side side, bool utf8,
                       const std::string_view* chars = nullptr);
std::string replace(std::string_view text, std::string_view old,
                    std::string_view replacement, std::ptrdiff_t limit, bool utf8);
std::vector<std::string> split(std::string_view text, const std::string_view* delimiter,
                               std::ptrdiff_t limit);
std::string indent(std::string_view text, std::size_t spaces, bool utf8 = true);
std::string dedent(std::string_view text, bool utf8 = true);
std::string_view newline_for(std::string_view style);
std::string normalize_newlines(std::string_view text, std::string_view newline);
std::string squish(std::string_view text);
std::string base64(std::string_view bytes);
std::string hex(std::string_view bytes);
std::string sha256(std::string_view bytes);

} // namespace syntwine::strings
