#pragma once

#include "syntwine/detail/platform/tokenizer.hpp"
#include "syntwine/detail/platform/utf8.hpp"

#include <pybind11/pybind11.h>

namespace syntwine {

struct Budget {
    std::optional<std::size_t> characters, bytes, tokens;
};

struct Usage {
    std::optional<std::size_t> characters;
    std::size_t bytes{};
    std::optional<std::size_t> tokens;
};

void bind_budget(pybind11::module_& module);

} // namespace syntwine
