#pragma once

#include <filesystem>
#include <string_view>

namespace syntwine {

void set_tokenizer_data_dir(std::filesystem::path directory);
// Input has already been checked by utf8_characters().
std::size_t token_count(std::string_view utf8, std::string_view encoding);

} // namespace syntwine
