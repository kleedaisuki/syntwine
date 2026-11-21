#pragma once

#include "core.hpp"

namespace syntwine {

void bind_markdown(py::module_&);
MD markdown_codeblock(const Source&, std::string language);
std::string markdown_squish(std::string_view source);

} // namespace syntwine
