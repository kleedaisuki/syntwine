#include "json.hpp"
#include "utf8.hpp"

#include <jsoncons_ext/jsonpath/jsonpath.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace syntwine {
namespace {

using Json = JsonTree;
using Tag = jsoncons::semantic_tag;

py::object json_to_python(const Json& value) {
    switch (value.tag()) {
    case Tag::bigint:
        return py::module_::import("builtins").attr("int")(py::str(value.as_string_view()));
    case Tag::bigdec:
        return py::module_::import("builtins").attr("float")(py::str(value.as_string_view()));
    default:
        break;
    }
    switch (value.type()) {
    case jsoncons::json_type::object: {
        py::dict result;
        for (const auto& item : value.object_range())
            result[py::str(item.key())] = json_to_python(item.value());
        return result;
    }
    case jsoncons::json_type::array: {
        py::list result;
        for (const auto& element : value.array_range())
            result.append(json_to_python(element));
        return result;
    }
    case jsoncons::json_type::string: return py::str(value.as_string_view());
    case jsoncons::json_type::boolean: return py::bool_(value.as<bool>());
    case jsoncons::json_type::int64: return py::int_(value.as<std::int64_t>());
    case jsoncons::json_type::uint64: return py::int_(value.as<std::uint64_t>());
    case jsoncons::json_type::float64: return py::float_(value.as<double>());
    case jsoncons::json_type::null: return py::none();
    default:
        throw Error(ErrorCode::type, "unsupported native JSON value");
    }
}

struct JsonValue final : FormatValue {
    JsonValue(Json value, std::optional<std::string> original, std::optional<unsigned> indent = {})
        : JsonValue(std::make_shared<const Json>(std::move(value)), std::move(original), indent) {}
    JsonValue(std::shared_ptr<const Json> value, std::optional<std::string> original,
              std::optional<unsigned> indent = {})
        : tree(std::move(value)), source(std::move(original)), indentation(indent) {}
    Kind kind() const noexcept override { return Kind::json; }
    std::string serialize() const override {
        if (source)
            return *source;
        std::string result;
        try {
            if (indentation) {
                // jsoncons uses an 8-bit indent width; produce general widths without a lossy cast.
                const auto width = *indentation > 255 ? 1 : static_cast<std::uint8_t>(*indentation);
                tree->dump(result, jsoncons::json_options{}.indent_size(width), jsoncons::indenting::indent);
                if (*indentation > 255)
                    result = widen_indent(result, *indentation);
            } else {
                tree->dump(result);
            }
        } catch (const jsoncons::json_exception& e) {
            throw Error(ErrorCode::serialization, e.what());
        }
        return result;
    }
    py::object to_python() const override { return json_to_python(*tree); }

    static std::string widen_indent(std::string_view encoded, unsigned width) {
        std::string result;
        for (std::size_t pos = 0; pos < encoded.size();) {
            const auto start = pos;
            while (pos < encoded.size() && encoded[pos] == ' ')
                ++pos;
            const auto end = encoded.find('\n', pos);
            result.append((pos - start) * width, ' ');
            if (end == std::string_view::npos) {
                result.append(encoded.substr(pos));
                break;
            }
            result.append(encoded.substr(pos, end - pos + 1));
            pos = end + 1;
        }
        return result;
    }

    std::shared_ptr<const Json> tree;
    std::optional<std::string> source;
    std::optional<unsigned> indentation;
};

const JsonValue& json_document(const Value& value) {
    if (const auto* native = std::get_if<std::shared_ptr<const FormatValue>>(&value))
        if (const auto* json = dynamic_cast<const JsonValue*>(native->get()))
            return *json;
    throw Error(ErrorCode::type, "expected a JSON value");
}

Value parse_json(std::string_view source) {
    if (!utf8_characters(source))
        throw Error(ErrorCode::parse, "JSON source must be valid UTF-8");
    try {
        auto tree = Json::parse(source, jsoncons::json_options{}.allow_comments(false));
        return make_json(std::move(tree), std::string(source));
    } catch (const jsoncons::ser_error& e) {
        throw Error(ErrorCode::parse, e.what(),
                    std::to_string(e.line()) + ":" + std::to_string(e.column()));
    }
}

std::string json_pyscalar_string(py::handle value) {
    return std::get<Utf8>(from_python(value)).data();
}

Json json_from_python_impl(py::handle value, std::unordered_set<PyObject*>& stack);

Json json_container(py::handle value, std::unordered_set<PyObject*>& stack) {
    if (py::isinstance<py::dict>(value)) {
        Json result(jsoncons::json_object_arg);
        for (const auto& [key, child] : py::reinterpret_borrow<py::dict>(value)) {
            if (!py::isinstance<py::str>(key))
                throw Error(ErrorCode::type, "JSON object keys must be str");
            result.try_emplace(json_pyscalar_string(key), json_from_python_impl(child, stack));
        }
        return result;
    }
    Json result(jsoncons::json_array_arg);
    auto sequence = py::reinterpret_borrow<py::sequence>(value);
    result.reserve(sequence.size());
    for (auto child : sequence)
        result.push_back(json_from_python_impl(child, stack));
    return result;
}

Json json_integer(py::handle value) {
    int overflow = 0;
    const auto signed_value = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
    if (PyErr_Occurred())
        throw py::error_already_set();
    if (!overflow)
        return signed_value;
    if (overflow > 0) {
        const auto unsigned_value = PyLong_AsUnsignedLongLong(value.ptr());
        if (!PyErr_Occurred())
            return unsigned_value;
        PyErr_Clear();
    }
    // A Python int subclass may override __str__, so normalize before converting.
    const auto as_int = py::module_::import("builtins").attr("int")(value);
    return Json(py::cast<std::string>(py::str(as_int)), Tag::bigint);
}

Json json_from_python_impl(py::handle value, std::unordered_set<PyObject*>& stack) {
    if (value.is_none())
        return Json::null();
    if (py::isinstance<py::bool_>(value))
        return py::cast<bool>(value);
    if (py::isinstance<py::int_>(value))
        return json_integer(value);
    if (py::isinstance<py::float_>(value)) {
        const auto number = py::cast<double>(value);
        if (!std::isfinite(number))
            throw Error(ErrorCode::serialization, "JSON has no non-finite numbers");
        return number;
    }
    if (py::isinstance<py::str>(value))
        return Json(json_pyscalar_string(value));
    if (!py::isinstance<py::dict>(value) && !py::isinstance<py::list>(value) &&
        !py::isinstance<py::tuple>(value))
        throw Error(ErrorCode::type, "expected JSON-compatible mapping, sequence or scalar");
    if (stack.size() >= 1024 || !stack.insert(value.ptr()).second)
        throw Error(ErrorCode::type, "cyclic or excessively nested JSON value");
    auto result = json_container(value, stack);
    stack.erase(value.ptr());
    return result;
}

Json json_from_value(const Value& value) {
    if (const auto* formatted = std::get_if<std::shared_ptr<const FormatValue>>(&value);
        formatted && (*formatted)->kind() == Kind::json)
        return json_tree(value);
    if (const auto* text = std::get_if<Utf8>(&value))
        return Json(text->data());
    if (std::holds_alternative<std::monostate>(value))
        return Json::null();
    if (const auto* object = std::get_if<py::object>(&value)) {
        py::gil_scoped_acquire gil;
        return json_from_python(*object);
    }
    throw Error(ErrorCode::type, "expected JSON-compatible value");
}

struct JsonKey {
    std::variant<std::string, std::int64_t> value;

    std::optional<std::size_t> index(std::size_t size) const {
        const auto* number = std::get_if<std::int64_t>(&value);
        if (!number)
            throw Error(ErrorCode::type, "JSON array indexing requires an integer");
        if (*number >= 0)
            return static_cast<std::uint64_t>(*number) < size
                ? std::optional(static_cast<std::size_t>(*number)) : std::nullopt;
        const auto from_end = std::uint64_t{0} - static_cast<std::uint64_t>(*number);
        return from_end <= size ? std::optional(size - from_end) : std::nullopt;
    }

    const std::string& name() const {
        if (const auto* str = std::get_if<std::string>(&value))
            return *str;
        throw Error(ErrorCode::type, "JSON object indexing requires a string");
    }

    std::string display() const {
        if (const auto* number = std::get_if<std::int64_t>(&value))
            return "[" + std::to_string(*number) + "]";
        return std::get<std::string>(value);
    }
};

JsonKey json_key(const Value& key) {
    auto v = json_from_value(key);
    if (json_string(v))
        return {v.as_string()};
    if (v.is_int64())
        return {v.as<std::int64_t>()};
    throw Error(ErrorCode::type, "JSON key must be a string or integer");
}

const Json* get(const Json& tree, const JsonKey& key) {
    if (tree.is_object()) {
        const auto it = tree.find(key.name());
        return it == tree.object_range().end() ? nullptr : &it->value();
    }
    if (tree.is_array()) {
        const auto index = key.index(tree.size());
        return index ? &tree[*index] : nullptr;
    }
    throw Error(ErrorCode::type, "expected a JSON array or object");
}

void assign(Json& tree, const JsonKey& key, Json value) {
    if (tree.is_object()) {
        tree.insert_or_assign(key.name(), std::move(value));
        return;
    }
    if (tree.is_array()) {
        if (const auto index = key.index(tree.size())) {
            tree[*index] = std::move(value);
            return;
        }
        throw Error(ErrorCode::operation, "JSON array index out of bounds");
    }
    throw Error(ErrorCode::type, "expected a JSON array or object");
}

void remove(Json& tree, const JsonKey& key) {
    if (tree.is_object()) {
        tree.erase(key.name());
        return;
    }
    if (tree.is_array()) {
        if (const auto index = key.index(tree.size()))
            tree.erase(tree.array_range().begin() + static_cast<std::ptrdiff_t>(*index));
        return;
    }
    throw Error(ErrorCode::type, "expected a JSON array or object");
}

void merge(Json& left, const Json& right, std::string_view policy,
           std::vector<std::string_view>& path) {
    if (left.is_object() && right.is_object()) {
        for (const auto& item : right.object_range()) {
            const auto key = item.key();
            auto it = left.find(key);
            if (it == left.object_range().end())
                left.try_emplace(key, item.value());
            else {
                path.push_back(key);
                merge(it->value(), item.value(), policy, path);
                path.pop_back();
            }
        }
        return;
    }
    if (policy == "right")
        left = right;
    else if (policy == "error" && left != right) {
        std::string message = "conflicting JSON merge values at $";
        for (const auto key : path) {
            message += '[';
            Json(key).dump(message);
            message += ']';
        }
        throw Error(ErrorCode::operation, std::move(message));
    }
}

Json sorted_keys(const Json& tree) {
    if (tree.is_array()) {
        Json result(jsoncons::json_array_arg);
        result.reserve(tree.size());
        for (const auto& element : tree.array_range())
            result.push_back(sorted_keys(element));
        return result;
    }
    if (!tree.is_object())
        return tree;
    std::vector<std::string_view> keys;
    keys.reserve(tree.size());
    for (const auto& item : tree.object_range())
        keys.emplace_back(item.key());
    std::ranges::sort(keys);
    Json result(jsoncons::json_object_arg);
    for (auto key : keys)
        result.try_emplace(key, sorted_keys(tree.at(key)));
    return result;
}

void append_dependencies(std::vector<NodePtr>& refs, py::handle value) {
    auto operands = dependencies(value);
    refs.insert(refs.end(), operands.begin(), operands.end());
}

NodePtr json_get(const JSON& source, py::object key, const py::args& defaults, Kind result_type) {
    if (defaults.size() > 1)
        throw py::type_error("get accepts at most one default");
    auto refs = std::vector<NodePtr>{source.node};
    append_dependencies(refs, key);
    append_dependencies(refs, defaults);
    py::object default_value = defaults.empty() ? py::object{} : snapshot(defaults[0]);
    return make_node(result_type == Kind::text ? "json.get_text" : "json.get", result_type,
        std::move(refs),
        [node = source.node, key = std::move(key), default_value = std::move(default_value), result_type]
        (Session& s) -> Value {
            const auto input = s.eval(node);
            const auto& doc = json_document(input);
            const auto resolved_key = json_key(s.resolve(key));
            const auto* result = get(*doc.tree, resolved_key);
            if (result) {
                if (result_type == Kind::json)
                    return std::make_shared<const JsonValue>(
                        std::shared_ptr<const Json>(doc.tree, result), std::nullopt);
                if (!json_string(*result))
                    throw Error(ErrorCode::type, "JSON value is not a string");
                return Utf8(result->as_string());
            }
            if (!default_value)
                throw Error(ErrorCode::operation, "JSON key or index not found: " + resolved_key.display());
            auto fallback = s.resolve(default_value);
            if (result_type == Kind::json)
                return make_json(json_from_value(fallback));
            if (!std::holds_alternative<Utf8>(fallback))
                throw Error(ErrorCode::type, "JSON get_text default must be text");
            return fallback;
        });
}

NodePtr json_filter(const JSON& source, py::object fn) {
    return make_node("json.filter", Kind::json, {source.node},
        [node = source.node, fn = std::move(fn)](Session& session) {
            const auto input = session.eval(node);
            const auto& tree = json_tree(input);
            auto keep = [&](const Json& value) {
                py::gil_scoped_acquire gil;
                const auto result = session.call(fn, py::make_tuple(json_to_python(value)), py::dict{}, "json.filter");
                const auto truth = PyObject_IsTrue(result.ptr());
                if (truth < 0)
                    throw py::error_already_set();
                return truth != 0;
            };
            if (tree.is_array()) {
                Json out(jsoncons::json_array_arg);
                for (const auto& item : tree.array_range())
                    if (keep(item))
                        out.push_back(item);
                return make_json(std::move(out));
            }
            if (tree.is_object()) {
                Json out(jsoncons::json_object_arg);
                for (const auto& item : tree.object_range())
                    if (keep(item.value()))
                        out.try_emplace(item.key(), item.value());
                return make_json(std::move(out));
            }
            throw Error(ErrorCode::type, "JSON filter requires an array or object");
        }, false);
}

} // namespace

Value make_json(JsonTree value, std::optional<std::string> source) {
    return std::make_shared<const JsonValue>(std::move(value), std::move(source));
}

const JsonTree& json_tree(const Value& value) {
    return *json_document(value).tree;
}

JsonTree json_from_python(py::handle value) {
    std::unordered_set<PyObject*> stack;
    return json_from_python_impl(value, stack);
}

std::string minify_valid_json(std::string_view source) {
    if (source.starts_with("\xef\xbb\xbf"))
        source.remove_prefix(3);
    std::string result;
    result.reserve(source.size());
    bool quoted = false;
    bool escaped = false;
    for (char c : source) {
        if (quoted) {
            result.push_back(c);
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            result.push_back(c);
            quoted = c == '"';
        }
    }
    return result;
}

void bind_json(py::module_& module) {
    register_format(Kind::json, [](std::string_view input) {
        py::gil_scoped_release release;
        return parse_json(input);
    }, [](py::handle value) {
        return make_json(json_from_python(value));
    });
    py::class_<JSON, Source>(module, "JSON")
        .def(py::init([](py::str input) {
            return JSON(make_node("json.parse", Kind::json, {},
                [input = std::move(input)](Session& s) {
                    const auto raw = s.resolve(input);
                    py::gil_scoped_release release;
                    return parse_json(std::get<Utf8>(raw).data());
                }, true, {}, {}, "parse.json"));
        }), py::arg("value"))
        .def_static("from_value", [](py::object value) {
            value = snapshot(value);
            auto refs = dependencies(value);
            return JSON(make_node("json.from_value", Kind::json, std::move(refs),
                [value = std::move(value)](Session& s) { return make_json(json_from_value(s.resolve(value))); }));
        }, py::arg("value"))
        .def("get", [](const JSON& self, py::object key, const py::args& defaults) {
            return JSON(json_get(self, std::move(key), defaults, Kind::json));
        }, py::arg("key"))
        .def("get_text", [](const JSON& self, py::object key, const py::args& defaults) {
            return Text(json_get(self, std::move(key), defaults, Kind::text));
        }, py::arg("key"))
        .def("set", [](const JSON& self, py::object key, py::object value) {
            value = snapshot(value);
            auto refs = std::vector<NodePtr>{self.node};
            append_dependencies(refs, key);
            append_dependencies(refs, value);
            return JSON(make_node("json.set", Kind::json, std::move(refs),
                [node = self.node, key = std::move(key), value = std::move(value)](Session& s) {
                    auto tree = json_tree(s.eval(node));
                    assign(tree, json_key(s.resolve(key)), json_from_value(s.resolve(value)));
                    return make_json(std::move(tree));
                }));
        }, py::arg("key"), py::arg("value"))
        .def("remove", [](const JSON& self, py::object key) {
            auto refs = std::vector<NodePtr>{self.node};
            append_dependencies(refs, key);
            return JSON(make_node("json.remove", Kind::json, std::move(refs),
                [node = self.node, key = std::move(key)](Session& s) {
                    auto tree = json_tree(s.eval(node));
                    remove(tree, json_key(s.resolve(key)));
                    return make_json(std::move(tree));
                }));
        }, py::arg("key"))
        .def("merge", [](const JSON& self, py::object other, std::string conflict) {
            other = snapshot(other);
            auto refs = std::vector<NodePtr>{self.node};
            append_dependencies(refs, other);
            return JSON(make_node("json.merge", Kind::json, std::move(refs),
                [node = self.node, other = std::move(other), conflict = std::move(conflict)](Session& s) {
                    if (conflict != "left" && conflict != "right" && conflict != "error")
                        throw Error(ErrorCode::operation, "JSON conflict policy must be left, right or error");
                    const auto input = s.eval(node);
                    auto incoming = json_from_value(s.resolve(other));
                    py::gil_scoped_release release;
                    auto tree = json_tree(input);
                    std::vector<std::string_view> path;
                    merge(tree, incoming, conflict, path);
                    return make_json(std::move(tree));
                }, true, {}, {}, "merge"));
        }, py::arg("other"), py::kw_only(), py::arg("conflict") = "right")
        .def("select", [](const JSON& self, py::object path) {
            auto refs = std::vector<NodePtr>{self.node};
            append_dependencies(refs, path);
            return JSON(make_node("json.select", Kind::json, std::move(refs),
                [node = self.node, path = std::move(path)](Session& s) {
                    const auto input = s.eval(node);
                    const auto query = s.resolve(path);
                    const auto* text = std::get_if<Utf8>(&query);
                    if (!text)
                        throw Error(ErrorCode::type, "JSONPath expression must be a string");
                    py::gil_scoped_release release;
                    try {
                        return make_json(jsoncons::jsonpath::json_query(json_tree(input), text->data()));
                    } catch (const jsoncons::jsonpath::jsonpath_error& e) {
                        throw Error(ErrorCode::operation, e.what(),
                                    std::to_string(e.line()) + ":" + std::to_string(e.column()));
                    }
                }));
        }, py::arg("path"))
        .def("filter", [](const JSON& self, py::object predicate) {
            return JSON(json_filter(self, std::move(predicate)));
        }, py::arg("predicate"))
        .def("append_array", [](const JSON& self, py::args values) {
            auto refs = std::vector<NodePtr>{self.node};
            auto frozen = snapshot(values);
            append_dependencies(refs, frozen);
            return JSON(make_node("json.append_array", Kind::json, std::move(refs),
                [node = self.node, values = std::move(frozen)](Session& s) {
                    auto tree = json_tree(s.eval(node));
                    if (!tree.is_array())
                        throw Error(ErrorCode::type, "JSON append_array requires an array");
                    for (const auto item : py::reinterpret_borrow<py::tuple>(values))
                        tree.push_back(json_from_value(s.resolve(item)));
                    return make_json(std::move(tree));
                }));
        })
        .def("sort_keys", [](const JSON& self) {
            return JSON(make_node("json.sort_keys", Kind::json, {self.node}, [node = self.node](Session& s) {
                const auto input = s.eval(node);
                py::gil_scoped_release release;
                return make_json(sorted_keys(json_tree(input)));
            }));
        })
        .def("pretty", [](const JSON& self, unsigned indent) {
            return JSON(make_node("json.pretty", Kind::json, {self.node},
                [node = self.node, indent](Session& s) -> Value {
                    return std::make_shared<const JsonValue>(json_document(s.eval(node)).tree, std::nullopt, indent);
                }));
        }, py::arg("indent") = 2)
        .def("minify", [](const JSON& self) {
            return JSON(make_node("json.minify", Kind::json, {self.node}, [node = self.node](Session& s) {
                const auto input = s.eval(node);
                py::gil_scoped_release release;
                const auto& json = json_document(input);
                std::optional<std::string> compact;
                if (json.source)
                    compact = minify_valid_json(*json.source);
                return Value(std::make_shared<const JsonValue>(json.tree, std::move(compact)));
            }));
        })
        .def("to_toml", [](const JSON& self) {
            return TOML(make_node("json.to_toml", Kind::toml, {self.node}, [node = self.node](Session& s) {
                return toml_from_json(json_tree(s.eval(node)));
            }));
        });
}

} // namespace syntwine
