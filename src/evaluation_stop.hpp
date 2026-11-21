#pragma once

#include "core.hpp"

namespace syntwine {

// A terminal evaluator limit propagates without copying the diagnostic at each frame.
struct EvaluationStop {
    Error error;
};

} // namespace syntwine
