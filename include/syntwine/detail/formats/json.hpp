#pragma once

#include "syntwine/detail/core/core.hpp"

#include <jsoncons/json.hpp>

namespace syntwine {

using JsonTree = jsoncons::ojson;

inline bool json_string(const JsonTree& value) noexcept {
    const auto tag = value.tag();
    // The parser records ordinary strings that contain no escapes with a separate storage tag.
    return value.is_string() && (tag == jsoncons::semantic_tag::none || tag == jsoncons::semantic_tag::noesc);
}

Value make_json(JsonTree value, std::optional<std::string> source = std::nullopt);
const JsonTree& json_tree(const Value& value);
JsonTree json_from_python(py::handle value);
Value toml_from_json(const JsonTree& json);
std::string minify_valid_json(std::string_view source);

void bind_json(py::module_& module);

} // namespace syntwine
