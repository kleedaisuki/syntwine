#include "syntwine/detail/formats/text.hpp"

#include "syntwine/detail/formats/text_algorithms.hpp"

#include <utility>

namespace syntwine {
namespace {

template <class Buffer>
constexpr Kind string_kind = std::is_same_v<Buffer, Utf8> ? Kind::text : Kind::literal;

template <class Buffer>
const std::string&
get_string(const Value& value) {
    if (const auto* buffer = std::get_if<Buffer>(&value))
        return buffer->data();
    throw Error(ErrorCode::type, "expected " + std::string(kind_name(string_kind<Buffer>)));
}

template <class Buffer>
class Operand {
public:
    explicit Operand(py::handle object) {
        if (py::isinstance<Source>(object)) {
            node_ = object.cast<const Source&>().node;
        } else if constexpr (std::is_same_v<Buffer, Utf8>) {
            if (!PyUnicode_Check(object.ptr()))
                throw Error(ErrorCode::type, "expected Text or str");
            constant_ = py::reinterpret_borrow<py::object>(object);
        } else {
            if (!PyBytes_Check(object.ptr()))
                throw Error(ErrorCode::type, "expected Literal or bytes");
            constant_ = Bytes{object.cast<std::string>()};
        }
    }

    Buffer
    get(Session& session) const {
        if (!node_) {
            if constexpr (std::is_same_v<Buffer, Bytes>)
                return *constant_;
            else
                return checked(from_python(constant_, string_kind<Buffer>));
        }
        return checked(session.eval(node_));
    }

    void
    add_dependency(std::vector<NodePtr>& deps) const {
        if (node_)
            deps.push_back(node_);
    }

private:
    static Buffer
    checked(Value value) {
        get_string<Buffer>(value);
        return std::get<Buffer>(std::move(value));
    }

    std::conditional_t<std::is_same_v<Buffer, Bytes>, std::optional<Buffer>, py::object> constant_;
    NodePtr node_;
};

template <class Output, class Input, class Fn>
Output
transform(const Input& source, std::string name, Kind kind, Fn fn, std::vector<NodePtr> deps = {}) {
    using Buffer = std::conditional_t<std::is_same_v<Input, Text>, Utf8, Bytes>;
    deps.insert(deps.begin(), source.node);
    return Output{make_node(
        std::move(name),
        kind,
        std::move(deps),
        [input = source.node, fn = std::move(fn)](Session& session) {
            auto value = session.eval(input);
            return fn(session, get_string<Buffer>(value));
        }
    )};
}

template <class Output, class Input, class Fn>
Output
native(const Input& input, std::string name, Kind kind, Fn fn) {
    return transform<Output>(
        input,
        std::move(name),
        kind,
        [fn = std::move(fn)](Session&, std::string_view data) -> Value {
            py::gil_scoped_release release;
            return fn(data);
        }
    );
}

template <class Input, class Buffer>
Input
concatenate(const Input& source, py::iterable parts, bool join) {
    std::vector<Operand<Buffer>> operands;
    std::vector<NodePtr> deps;
    for (const auto item : parts) {
        operands.emplace_back(item);
        operands.back().add_dependency(deps);
    }
    return transform<Input>(
        source,
        join ? "text.join" : "concat",
        string_kind<Buffer>,
        [operands = std::move(operands), join](Session& session, std::string_view input) -> Value {
            std::vector<Buffer> values;
            values.reserve(operands.size());
            for (const auto& operand : operands)
                values.push_back(operand.get(session));

            py::gil_scoped_release release;
            std::string output;
            std::size_t size = join ? 0 : input.size();
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto extra = values[i].data().size();
                if (extra > output.max_size() - size)
                    throw std::length_error("concatenated result exceeds maximum string length");
                size += extra;
                if (join && i != 0) {
                    if (input.size() > output.max_size() - size)
                        throw std::length_error("joined result exceeds maximum string length");
                    size += input.size();
                }
            }
            output.reserve(size);
            if (!join)
                output.append(input);
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (join && i != 0)
                    output.append(input);
                output.append(values[i].data());
            }
            return Buffer{std::move(output)};
        },
        std::move(deps)
    );
}

template <class Input, class Buffer>
Input
select_slice(const Input& source, py::object start, py::object stop, py::object step) {
    return transform<Input>(
        source,
        "slice",
        string_kind<Buffer>,
        [slice = py::slice(std::move(start), std::move(stop), std::move(step))](
            Session&,
            std::string_view data
        ) -> Value {
            constexpr bool utf8 = std::is_same_v<Buffer, Utf8>;
            std::size_t size;
            {
                py::gil_scoped_release release;
                size = utf8 ? strings::count(data) : data.size();
            }
            py::ssize_t begin, end, stride, length;
            if (!slice.compute(static_cast<py::ssize_t>(size), &begin, &end, &stride, &length))
                throw py::error_already_set();
            py::gil_scoped_release release;
            return Buffer{strings::slice(
                data,
                size != data.size(),
                begin,
                stride,
                static_cast<std::size_t>(length)
            )};
        }
    );
}

template <class Input, class Buffer>
Input
substitute(const Input& source, py::handle old, py::handle replacement, std::ptrdiff_t limit) {
    Operand<Buffer> from(old), to(replacement);
    std::vector<NodePtr> deps;
    from.add_dependency(deps);
    to.add_dependency(deps);
    return transform<Input>(
        source,
        "replace",
        string_kind<Buffer>,
        [from = std::move(from),
         to = std::move(to),
         limit](Session& session, std::string_view data) -> Value {
            auto before = from.get(session);
            auto after = to.get(session);
            py::gil_scoped_release release;
            return Buffer{strings::replace(
                data,
                before.data(),
                after.data(),
                limit,
                std::is_same_v<Buffer, Utf8>
            )};
        },
        std::move(deps)
    );
}

Source
boundary(const Literal& source, py::handle argument, bool prefix) {
    Operand<Bytes> operand(argument);
    std::vector<NodePtr> deps;
    operand.add_dependency(deps);
    return transform<Source>(
        source,
        prefix ? "literal.starts_with" : "literal.ends_with",
        Kind::any,
        [operand = std::move(operand), prefix](Session& session, std::string_view data) -> Value {
            auto other = operand.get(session);
            return py::bool_(
                prefix ? data.starts_with(other.data()) : data.ends_with(other.data())
            );
        },
        std::move(deps)
    );
}

Text
strip(const Text& source, py::handle chars, strings::Side side) {
    std::optional<Operand<Utf8>> operand;
    std::vector<NodePtr> deps;
    if (!chars.is_none()) {
        operand.emplace(chars);
        operand->add_dependency(deps);
    }
    const auto name = side == strings::Side::left    ? "text.lstrip"
                      : side == strings::Side::right ? "text.rstrip"
                                                     : "text.strip";
    return transform<Text>(
        source,
        name,
        Kind::text,
        [operand = std::move(operand), side](Session& session, std::string_view data) -> Value {
            auto value = operand ? std::optional(operand->get(session)) : std::nullopt;
            auto set = value ? std::string_view(value->data()) : std::string_view{};
            py::gil_scoped_release release;
            return Utf8{std::string(strings::strip(data, side, true, value ? &set : nullptr))};
        },
        std::move(deps)
    );
}

Source
split(const Text& source, py::handle separator, std::ptrdiff_t limit) {
    std::optional<Operand<Utf8>> operand;
    std::vector<NodePtr> deps;
    if (!separator.is_none()) {
        operand.emplace(separator);
        operand->add_dependency(deps);
    }
    return transform<Source>(
        source,
        "text.split",
        Kind::any,
        [operand = std::move(operand), limit](Session& session, std::string_view data) -> Value {
            auto value = operand ? std::optional(operand->get(session)) : std::nullopt;
            auto delimiter = value ? std::string_view(value->data()) : std::string_view{};
            std::vector<std::string> parts;
            {
                py::gil_scoped_release release;
                parts = strings::split(data, value ? &delimiter : nullptr, limit);
            }
            py::list result(parts.size());
            for (std::size_t i = 0; i < parts.size(); ++i)
                result[i] = py::str(parts[i]);
            return result;
        },
        std::move(deps)
    );
}

bool
decimal(std::string_view word) {
    if (word.empty())
        return false;
    bool ascii = true;
    for (auto character : word) {
        if (static_cast<unsigned char>(character) >= 0x80)
            ascii = false;
        else if (character < '0' || character > '9')
            return false;
    }
    // CPython's formatting syntax also accepts non-ASCII decimal digits.
    return ascii || py::str(word).attr("isdecimal")().cast<bool>();
}

py::object
require_object(PyObject* object) {
    if (!object)
        throw py::error_already_set();
    return py::reinterpret_steal<py::object>(object);
}

py::object
decimal_object(std::string_view word) {
    auto text = py::str(word);
    return require_object(PyLong_FromUnicodeObject(text.ptr(), 10));
}

struct Formatter {
    const py::tuple& args;
    const py::dict& kwargs;
    std::size_t next_auto = 0;
    enum class IndexMode { unused, automatic, manual };
    IndexMode index_mode = IndexMode::unused;

    py::object
    head(std::string_view name) {
        if (name.empty()) {
            if (index_mode == IndexMode::manual)
                throw Error(
                    ErrorCode::operation,
                    "cannot switch from manual to automatic field numbering"
                );
            index_mode = IndexMode::automatic;
            if (next_auto >= args.size())
                throw py::index_error("replacement index out of range");
            return args[next_auto++];
        }
        if (!decimal(name))
            return kwargs[py::str(name)];

        if (index_mode == IndexMode::automatic)
            throw Error(
                ErrorCode::operation,
                "cannot switch from automatic to manual field numbering"
            );
        index_mode = IndexMode::manual;
        const auto index = decimal_object(name).cast<std::size_t>();
        if (index >= args.size())
            throw py::index_error("replacement index out of range");
        return args[index];
    }

    py::object
    field(std::string_view name) {
        const auto root_end = name.find_first_of(".[");
        auto value = head(name.substr(0, root_end));
        if (root_end == std::string_view::npos)
            return value;
        name.remove_prefix(root_end);
        while (!name.empty()) {
            if (name.front() == '.') {
                const auto next = name.find_first_of(".[", 1);
                const auto attr = name.substr(1, next == std::string_view::npos ? next : next - 1);
                if (attr.empty())
                    throw Error(ErrorCode::operation, "empty attribute in format field");
                value = py::getattr(value, py::str(attr));
                name.remove_prefix(next == std::string_view::npos ? name.size() : next);
                continue;
            }
            if (name.front() != '[')
                throw Error(
                    ErrorCode::operation,
                    "expected attribute or item lookup in format field"
                );
            const auto end = name.find(']');
            if (end == std::string_view::npos || end == 1)
                throw Error(ErrorCode::operation, "invalid item lookup in format field");
            const auto item = name.substr(1, end - 1);
            const auto key = decimal(item) ? decimal_object(item) : py::object(py::str(item));
            value = require_object(PyObject_GetItem(value.ptr(), key.ptr()));
            name.remove_prefix(end + 1);
        }
        return value;
    }

    static std::size_t
    field_end(std::string_view pattern, std::size_t pos) {
        while (pos < pattern.size()) {
            auto c = pattern[pos];
            if (c == '}' || c == ':' || c == '!')
                break;
            if (c == '{')
                throw Error(ErrorCode::operation, "unexpected '{' in format field");
            if (c == '[') {
                pos = pattern.find(']', pos + 1);
                if (pos == std::string_view::npos)
                    throw Error(ErrorCode::operation, "unclosed item lookup in format field");
            }
            ++pos;
        }
        return pos;
    }

    static py::object
    convert(py::object value, char code) {
        if (code == 's')
            return require_object(PyObject_Str(value.ptr()));
        if (code == 'r')
            return require_object(PyObject_Repr(value.ptr()));
        if (code == 'a')
            return require_object(PyObject_ASCII(value.ptr()));
        throw Error(ErrorCode::operation, "invalid format conversion");
    }

    std::string
    replacement(std::string_view pattern, std::size_t& pos, unsigned depth) {
        const auto end = field_end(pattern, pos);
        auto value = field(pattern.substr(pos, end - pos));
        pos = end;
        if (pos < pattern.size() && pattern[pos] == '!') {
            if (++pos == pattern.size())
                throw Error(ErrorCode::operation, "missing format conversion");
            value = convert(std::move(value), pattern[pos++]);
        }
        std::string spec;
        if (pos < pattern.size() && pattern[pos] == ':') {
            const auto begin = ++pos;
            unsigned nesting = 0;
            for (; pos < pattern.size(); ++pos) {
                if (pattern[pos] == '{')
                    ++nesting;
                if (pattern[pos] != '}')
                    continue;
                if (nesting == 0)
                    break;
                --nesting;
            }
            const auto raw = pattern.substr(begin, pos - begin);
            spec = raw.find_first_of("{}") == std::string_view::npos ? std::string(raw)
                                                                     : render(raw, depth + 1);
        }
        if (pos == pattern.size() || pattern[pos++] != '}')
            throw Error(ErrorCode::operation, "missing '}' in format string");
        const auto format_spec = py::str(spec);
        return require_object(PyObject_Format(value.ptr(), format_spec.ptr())).cast<std::string>();
    }

    std::string
    render(std::string_view pattern, unsigned depth = 0) {
        if (depth > 1)
            throw Error(ErrorCode::operation, "nested format specifications exceed maximum depth");
        std::string output;
        output.reserve(pattern.size());
        for (std::size_t pos = 0; pos < pattern.size();) {
            const auto next = pattern.find_first_of("{}", pos);
            output.append(pattern.substr(pos, next - pos));
            if (next == std::string_view::npos)
                break;
            pos = next + 1;
            const auto brace = pattern[next];
            if (pos < pattern.size() && pattern[pos] == brace) {
                output.push_back(brace);
                ++pos;
            } else if (brace == '{') {
                output.append(replacement(pattern, pos, depth));
            } else {
                throw Error(ErrorCode::operation, "unexpected '}' in format string");
            }
        }
        return output;
    }
};

Text
format(const Text& source, py::args args, py::kwargs kwargs) {
    auto positional = py::reinterpret_borrow<py::tuple>(snapshot(args));
    auto keywords = py::reinterpret_borrow<py::dict>(snapshot(kwargs));
    auto deps = dependencies(positional);
    auto keyword_deps = dependencies(keywords);
    deps.insert(deps.end(), keyword_deps.begin(), keyword_deps.end());
    // Formatting may invoke user-defined item, attribute, and __format__ methods.
    deps.insert(deps.begin(), source.node);
    return Text{make_node(
        "text.format",
        Kind::text,
        std::move(deps),
        [input = source.node,
         positional = std::move(positional),
         keywords = std::move(keywords)](Session& session) -> Value {
            const auto value = session.eval(input);
            auto args = to_python(session.resolve(positional)).cast<py::tuple>();
            auto kwargs = to_python(session.resolve(keywords)).cast<py::dict>();
            return Utf8{Formatter{args, kwargs}.render(get_string<Utf8>(value))};
        },
        false
    )};
}

Literal
encode(const Text& source, std::string encoding) {
    if (encoding == "utf-8" || encoding == "UTF-8" || encoding == "utf8") {
        return Literal{make_node(
            "text.encode",
            Kind::literal,
            {source.node},
            [input = source.node](Session& session) -> Value {
                const auto text = session.eval(input);
                get_string<Utf8>(text);
                return Bytes{std::get<Utf8>(text)};
            }
        )};
    }
    return transform<Literal>(
        source,
        "text.encode",
        Kind::literal,
        [encoding = std::move(encoding)](Session&, std::string_view data) -> Value {
            if (encoding.find('\0') != std::string::npos)
                throw Error(ErrorCode::encoding, "encoding name contains a null byte");
            const auto text = py::str(data);
            auto* raw = PyUnicode_AsEncodedString(text.ptr(), encoding.c_str(), "strict");
            if (!raw) {
                py::error_already_set failure;
                auto error = Error(ErrorCode::encoding, failure.what());
                error.cause = failure.value();
                throw error;
            }
            return Bytes{require_object(raw).cast<std::string>()};
        }
    );
}

} // namespace

Literal
literal_source(py::bytes value) {
    auto buffer = Bytes{value.cast<std::string>()};
    return Literal{
        make_node("literal", Kind::literal, {}, [buffer = std::move(buffer)](Session&) -> Value {
            return buffer;
        })
    };
}

Text
text_source(py::str value) {
    return Text{make_node("text", Kind::text, {}, [value = std::move(value)](Session&) {
        return from_python(value, Kind::text);
    })};
}

void
bind_text(py::module_& module) {
    using namespace py::literals;
    py::class_<Literal, Source>(module, "Literal")
        .def(py::init(&literal_source), "value"_a.noconvert())
        .def(
            "slice",
            &select_slice<Literal, Bytes>,
            "start"_a = py::none(),
            "stop"_a = py::none(),
            "step"_a = py::none()
        )
        .def(
            "concat",
            [](const Literal& self, py::args parts) {
                return concatenate<Literal, Bytes>(self, parts, false);
            }
        )
        .def("replace_bytes", &substitute<Literal, Bytes>, "old"_a, "new"_a, "count"_a = -1)
        .def(
            "starts_with",
            [](const Literal& self, py::handle prefix) { return boundary(self, prefix, true); },
            "prefix"_a
        )
        .def(
            "ends_with",
            [](const Literal& self, py::handle suffix) { return boundary(self, suffix, false); },
            "suffix"_a
        )
        .def(
            "base64",
            [](const Literal& self) {
                return native<Text>(self, "literal.base64", Kind::text, [](std::string_view data) {
                    return Utf8{strings::base64(data)};
                });
            }
        )
        .def(
            "hex",
            [](const Literal& self) {
                return native<Text>(self, "literal.hex", Kind::text, [](std::string_view data) {
                    return Utf8{strings::hex(data)};
                });
            }
        )
        .def("sha256", [](const Literal& self) {
            return native<Text>(self, "literal.sha256", Kind::text, [](std::string_view data) {
                return Utf8{strings::sha256(data)};
            });
        });

    py::class_<Text, Source>(module, "Text")
        .def(py::init(&text_source), "value"_a.noconvert())
        .def(
            "concat",
            [](const Text& self, py::args parts) {
                return concatenate<Text, Utf8>(self, parts, false);
            }
        )
        .def(
            "join",
            [](const Text& self, py::iterable parts) {
                return concatenate<Text, Utf8>(self, parts, true);
            },
            "parts"_a
        )
        .def(
            "strip",
            [](const Text& self, py::handle chars) {
                return strip(self, chars, strings::Side::both);
            },
            "chars"_a = py::none()
        )
        .def(
            "lstrip",
            [](const Text& self, py::handle chars) {
                return strip(self, chars, strings::Side::left);
            },
            "chars"_a = py::none()
        )
        .def(
            "rstrip",
            [](const Text& self, py::handle chars) {
                return strip(self, chars, strings::Side::right);
            },
            "chars"_a = py::none()
        )
        .def("replace", &substitute<Text, Utf8>, "old"_a, "new"_a, "count"_a = -1)
        .def("split", &split, "sep"_a = py::none(), "maxsplit"_a = -1)
        .def(
            "slice",
            &select_slice<Text, Utf8>,
            "start"_a = py::none(),
            "stop"_a = py::none(),
            "step"_a = py::none()
        )
        .def(
            "indent",
            [](const Text& self, std::size_t spaces) {
                return native<Text>(
                    self,
                    "text.indent",
                    Kind::text,
                    [spaces](std::string_view data) { return Utf8{strings::indent(data, spaces)}; }
                );
            },
            "n"_a
        )
        .def(
            "dedent",
            [](const Text& self) {
                return native<Text>(self, "text.dedent", Kind::text, [](std::string_view data) {
                    return Utf8{strings::dedent(data)};
                });
            }
        )
        .def(
            "normalize_newlines",
            [](const Text& self, std::string style) {
                const auto newline = strings::newline_for(style);
                return native<Text>(
                    self,
                    "newline.normalize",
                    Kind::text,
                    [newline](std::string_view data) {
                        return Utf8{strings::normalize_newlines(data, newline)};
                    }
                );
            },
            "style"_a = "lf"
        )
        .def("format", &format)
        .def("encode", &encode, "encoding"_a = "utf-8");
}

} // namespace syntwine
