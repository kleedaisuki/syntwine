#pragma once

#include "core.hpp"

namespace syntwine {

Literal literal_source(py::bytes value);
Text text_source(py::str value);
void bind_text(py::module_& module);

} // namespace syntwine
