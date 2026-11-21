#pragma once

#include "syntwine/detail/core/core.hpp"

namespace syntwine {

struct Document : Source { using Source::Source; };

void bind_document(py::module_& module);

} // namespace syntwine
