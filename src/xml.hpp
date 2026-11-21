#pragma once

#include "core.hpp"

namespace syntwine {

void bind_xml(py::module_&);
std::string xml_squish(std::string_view source, std::string_view policy = "markup_only");

} // namespace syntwine
