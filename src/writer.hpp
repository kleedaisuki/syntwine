#pragma once

#include <filesystem>
#include <string_view>

namespace syntwine {

void atomic_write(const std::filesystem::path& path, std::string_view payload);

} // namespace syntwine
