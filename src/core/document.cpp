#include "syntwine/detail/core/document.hpp"

#include "syntwine/detail/core/budget.hpp"
#include "syntwine/detail/core/document_parts.hpp"
#include "syntwine/detail/formats/markdown.hpp"
#include "syntwine/detail/telemetry/telemetry.hpp"
#include "syntwine/detail/formats/text.hpp"
#include "syntwine/detail/formats/text_algorithms.hpp"
#include "syntwine/detail/formats/xml.hpp"

#include <algorithm>
#include <utility>

namespace syntwine {

std::string minify_valid_json(std::string_view source);

namespace {

enum class Format { raw, text, md, xml, json, toml };

Format format_of(std::string_view name) {
    if (name == "raw") return Format::raw;
    if (name == "text") return Format::text;
    if (name == "md" || name == "markdown") return Format::md;
    if (name == "xml") return Format::xml;
    if (name == "json") return Format::json;
    if (name == "toml") return Format::toml;
    throw py::value_error("unknown document format: " + std::string(name));
}

Kind kind_of(Format format) {
    switch (format) {
    case Format::md: return Kind::md;
    case Format::xml: return Kind::xml;
    case Format::json: return Kind::json;
    case Format::toml: return Kind::toml;
    case Format::raw: return Kind::literal;
    case Format::text: return Kind::text;
    }
    std::unreachable();
}

NodePtr part_node(py::handle part) {
    if (py::isinstance<Source>(part))
        return part.cast<const Source&>().node;
    if (PyUnicode_Check(part.ptr()))
        return text_source(py::reinterpret_borrow<py::str>(part)).node;
    if (PyBytes_Check(part.ptr()))
        return literal_source(py::reinterpret_borrow<py::bytes>(part)).node;
    throw Error(ErrorCode::type, "document parts and operands must be Source, str, or bytes");
}

Bytes serialize_part(Value value) {
    if (auto* bytes = std::get_if<Bytes>(&value))
        return std::move(*bytes);
    if (const auto* utf8 = std::get_if<Utf8>(&value))
        return Bytes{*utf8};
    return Bytes{serialize(value)};
}

bool truth(py::handle object) {
    if (py::isinstance<Source>(object))
        throw Error(ErrorCode::operation, "predicate must return a truth value, not a Source");
    const auto value = PyObject_IsTrue(object.ptr());
    if (value == -1)
        throw py::error_already_set();
    return value != 0;
}

struct PartStep {
    py::object fn;
    bool filter;
};

struct Post {
    std::string name;
    std::vector<NodePtr> dependencies;
    std::function<Bytes(Session&, const Bytes&)> compute;
    bool pure = true;
};

template<class T>
using SharedList = std::shared_ptr<const std::vector<T>>;

struct Layout final : NodeMetadata {
    explicit Layout(Format format) : format(format) {}

    Format format;
    NodePtr separator;
    std::shared_ptr<const DocumentParts> parts = std::make_shared<const DocumentParts>();
    SharedList<PartStep> part_steps = std::make_shared<const std::vector<PartStep>>();
    SharedList<Post> posts = std::make_shared<const std::vector<Post>>();
    std::optional<Budget> budget;

    void append_declared_dependencies(std::vector<NodePtr>& nodes) const override {
        parts->append_to(nodes);
    }
};

const Layout& layout(const Document& document) {
    return static_cast<const Layout&>(*document.node->metadata);
}

telemetry::Span span(const Session& session, std::string_view name,
                     telemetry::Level level = telemetry::Level::operations) {
    const auto* node = session.active();
    return telemetry::Span(session.trace(), name, level, node ? node->id : 0,
                           node ? std::string_view(node->source_id) : std::string_view{}, name);
}

std::optional<Bytes> evaluate_part(Session& session, const NodePtr& part,
                                      const std::vector<PartStep>& steps) {
    auto value = session.eval(part);
    for (const auto& step : steps) {
        auto result = session.call(step.fn, py::make_tuple(to_python(value)), py::dict(),
                                   step.filter ? "document.filter_parts" : "document.map_parts");
        if (!step.filter)
            value = session.resolve(result);
        else if (!truth(result))
            return std::nullopt;
    }

    telemetry::Span fragment(session.trace(), "fragment.serialize", telemetry::Level::operations,
                             part->id, part->source_id, "fragment.serialize");
    fragment.input_bytes(size_bytes(value));
    auto payload = serialize_part(std::move(value));
    fragment.output_bytes(payload.data().size());
    return payload;
}

std::string join_parts(const std::vector<Bytes>& parts, std::string_view separator) {
    std::string output;
    std::size_t size = 0;
    auto add_size = [&](std::size_t length) {
        if (length > output.max_size() - size)
            throw std::length_error("joined document exceeds maximum string length");
        size += length;
    };
    for (std::size_t i = 1; i < parts.size(); ++i)
        add_size(separator.size());
    for (const auto& part : parts)
        add_size(part.data().size());
    output.reserve(size);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
            output.append(separator);
        output.append(parts[i].data());
    }
    return output;
}

Bytes compose(Session& session, const Layout& current) {
    auto stage = span(session, "document.compose", telemetry::Level::stages);
    std::vector<Bytes> fragments;
    fragments.reserve(current.parts->size());
    current.parts->visit([&](const NodePtr& part) {
        if (auto item = evaluate_part(session, part, *current.part_steps))
            fragments.push_back(std::move(*item));
    });

    auto join = span(session, "parts.join");
    auto separator = current.format == Format::raw ? std::string_view{} : std::string_view("\n\n");
    std::optional<Bytes> resolved;
    if (current.separator && fragments.size() > 1) {
        resolved = serialize_part(session.eval(current.separator));
        separator = resolved->data();
    }

    auto output = [&]() -> Bytes {
        if (fragments.size() == 1)
            return std::move(fragments.front());
        py::gil_scoped_release release;
        return Bytes{join_parts(fragments, separator)};
    }();
    join.output_bytes(output.data().size());
    stage.output_bytes(output.data().size());
    return output;
}

void require_utf8(std::string_view input, ErrorCode code) {
    if (!utf8_characters(input))
        throw Error(code, "document content is not valid UTF-8");
}

void validate_document(Session& session, Format format, std::string_view bytes) {
    auto validation = span(session, "output.validate", telemetry::Level::stages);
    validation.input_bytes(bytes.size());
    try {
        if (format == Format::raw) {
            validation.output_bytes(bytes.size());
            return;
        }
        if (format == Format::text || format == Format::md) {
            // Every UTF-8 input is a CommonMark document; this need not allocate an AST.
            py::gil_scoped_release release;
            require_utf8(bytes, ErrorCode::serialization);
        } else {
            (void)parse(kind_of(format), bytes);
        }
    } catch (Error& error) {
        if (error.code == ErrorCode::decode || error.code == ErrorCode::parse)
            error.code = ErrorCode::serialization;
        validation.fail(kind_name_for_error(error.code));
        throw;
    }
    validation.output_bytes(bytes.size());
}

Value build(Session& session, const Layout& current) {
    auto output = compose(session, current);
    if (!current.posts->empty()) {
        auto stage = span(session, "document.post", telemetry::Level::stages);
        stage.input_bytes(output.data().size());
        for (const auto& operation : *current.posts) {
            auto step = span(session, operation.name);
            step.input_bytes(output.data().size());
            try {
                output = operation.compute(session, output);
                step.output_bytes(output.data().size());
            } catch (Error& error) {
                step.fail(kind_name_for_error(error.code));
                session.annotate(error);
                if (!error.dependency_path.empty() && error.dependency_path.back().node_id == session.active()->id)
                    error.dependency_path.back().operation = operation.name;
                throw;
            }
        }
        stage.output_bytes(output.data().size());
    }
    validate_document(session, current.format, output.data());
    if (current.budget)
        (void)session.validate_budget(output, *current.budget);
    return output;
}

Document make_document(std::shared_ptr<const Layout> current) {
    std::vector<NodePtr> deps;
    if (current->separator)
        deps.push_back(current->separator);
    bool pure = current->part_steps->empty();
    for (const auto& operation : *current->posts) {
        deps.insert(deps.end(), operation.dependencies.begin(), operation.dependencies.end());
        pure = pure && operation.pure;
    }
    auto compute = [current](Session& session) { return build(session, *current); };
    return Document{make_node("document", Kind::document, std::move(deps), std::move(compute),
                              pure, std::move(current))};
}

Document create(std::string format, py::handle separator) {
    auto current = std::make_shared<Layout>(format_of(format));
    if (!separator.is_none())
        current->separator = part_node(separator);
    return make_document(std::move(current));
}

Document with_parts(const Document& doc, DocumentParts parts) {
    auto current = std::make_shared<Layout>(layout(doc));
    current->parts = std::make_shared<const DocumentParts>(std::move(parts));
    return make_document(std::move(current));
}

Document append_one(const Document& doc, NodePtr part) {
    auto nodes = *layout(doc).parts;
    nodes.push(std::move(part));
    return with_parts(doc, std::move(nodes));
}

Document append(const Document& doc, py::iterable parts, bool front = false) {
    const auto& original = *layout(doc).parts;
    auto nodes = front ? DocumentParts{} : DocumentParts(original);
    for (const auto part : parts)
        nodes.push(part_node(part));
    if (front)
        original.visit([&](const NodePtr& part) { nodes.push(part); });
    return with_parts(doc, std::move(nodes));
}

py::ssize_t part_index(py::ssize_t index, std::size_t size, bool insert) {
    const auto count = static_cast<py::ssize_t>(size);
    if (index < 0)
        index += count;
    if (insert)
        return std::clamp(index, py::ssize_t{0}, count);
    if (index < 0 || index >= count)
        throw py::index_error("document part index out of range");
    return index;
}

Document insert(const Document& doc, py::ssize_t index, py::handle part) {
    const auto& original = *layout(doc).parts;
    const auto position = part_index(index, original.size(), true);
    auto node = part_node(part);
    if (static_cast<std::size_t>(position) == original.size())
        return append_one(doc, std::move(node));
    auto parts = original.materialize();
    parts.insert(parts.begin() + position, std::move(node));
    return with_parts(doc, DocumentParts(std::move(parts)));
}

Document remove(const Document& doc, py::ssize_t index) {
    const auto& original = *layout(doc).parts;
    const auto position = part_index(index, original.size(), false);
    auto parts = original.materialize();
    parts.erase(parts.begin() + position);
    return with_parts(doc, DocumentParts(std::move(parts)));
}

Document replace_part(const Document& doc, py::ssize_t index, py::handle part) {
    const auto& original = *layout(doc).parts;
    const auto position = static_cast<std::size_t>(part_index(index, original.size(), false));
    auto parts = original.materialize();
    parts[position] = part_node(part);
    return with_parts(doc, DocumentParts(std::move(parts)));
}

Document slice_parts(const Document& doc, py::object start, py::object stop) {
    const auto& parts = *layout(doc).parts;
    const py::slice bounds(std::move(start), std::move(stop), py::none());
    py::ssize_t begin, end, step, length;
    if (!bounds.compute(static_cast<py::ssize_t>(parts.size()), &begin, &end, &step, &length))
        throw py::error_already_set();
    DocumentParts selected;
    py::ssize_t index = 0;
    parts.visit([&](const NodePtr& part) {
        if (index >= begin && index < end)
            selected.push(part);
        ++index;
    });
    return with_parts(doc, std::move(selected));
}

Document join(const Document& doc, py::handle separator) {
    auto current = std::make_shared<Layout>(layout(doc));
    current->separator = separator.is_none() ? NodePtr{} : part_node(separator);
    return make_document(std::move(current));
}

Document concat(const Document& doc, const Document& other, py::handle separator) {
    auto current = std::make_shared<Layout>(layout(doc));
    auto parts = *current->parts;
    parts.push(other.node);
    current->parts = std::make_shared<const DocumentParts>(std::move(parts));
    if (!separator.is_none())
        current->separator = part_node(separator);
    return make_document(std::move(current));
}

Document part_step(const Document& doc, py::object fn, bool filter) {
    if (!PyCallable_Check(fn.ptr()))
        throw Error(ErrorCode::type, "document part callback must be callable");
    auto current = std::make_shared<Layout>(layout(doc));
    auto steps = *current->part_steps;
    steps.push_back({std::move(fn), filter});
    current->part_steps = std::make_shared<const std::vector<PartStep>>(std::move(steps));
    return make_document(std::move(current));
}

Document post(const Document& doc, Post operation) {
    auto current = std::make_shared<Layout>(layout(doc));
    auto posts = *current->posts;
    posts.push_back(std::move(operation));
    current->posts = std::make_shared<const std::vector<Post>>(std::move(posts));
    return make_document(std::move(current));
}

template<class Fn>
Document native_post(const Document& doc, std::string name, Fn fn) {
    return post(doc, Post{std::move(name), {},
        [fn = std::move(fn)](Session&, const Bytes& input) {
            py::gil_scoped_release release;
            return fn(input);
        }});
}

Bytes squish_bytes(const Bytes& input, Format format) {
    const auto& bytes = input.data();
    switch (format) {
    case Format::raw: return input;
    case Format::text:
        require_utf8(bytes, ErrorCode::decode);
        return Bytes{strings::squish(bytes)};
    case Format::md:
        require_utf8(bytes, ErrorCode::decode);
        return Bytes{markdown_squish(bytes)};
    case Format::xml: return Bytes{xml_squish(bytes, "markup_only")};
    case Format::json: return Bytes{minify_valid_json(bytes)};
    case Format::toml: return input;
    }
    std::unreachable();
}

Document squish(const Document& doc, std::string mode) {
    const auto format = mode == "auto" ? layout(doc).format : format_of(mode);
    return post(doc, Post{"squish", {}, [format](Session&, const Bytes& input) {
        // Whitespace removal must not turn invalid JSON into a valid value.
        if (format == Format::json || format == Format::toml)
            (void)parse(kind_of(format), input.data());
        py::gil_scoped_release release;
        return squish_bytes(input, format);
    }});
}

Document replace(const Document& doc, py::handle old, py::handle replacement, std::ptrdiff_t limit) {
    auto before = part_node(old);
    auto after = part_node(replacement);
    const bool utf8 = layout(doc).format != Format::raw;
    return post(doc, Post{"replace", {before, after},
        [before, after, limit, utf8](Session& session, const Bytes& input) {
            const auto from = serialize_part(session.eval(before));
            const auto to = serialize_part(session.eval(after));
            py::gil_scoped_release release;
            const auto scalar_boundaries = utf8 && from.data().empty()
                && utf8_characters(input.data()).has_value();
            return Bytes{strings::replace(input.data(), from.data(), to.data(), limit, scalar_boundaries)};
        }});
}

Document wrap(const Document& doc, py::handle prefix, py::handle suffix) {
    auto left = part_node(prefix);
    auto right = part_node(suffix);
    return post(doc, Post{"wrap", {left, right}, [left, right](Session& session, const Bytes& input) {
        const auto before = serialize_part(session.eval(left));
        const auto after = serialize_part(session.eval(right));
        py::gil_scoped_release release;
        return Bytes{std::string(before.data()).append(input.data()).append(after.data())};
    }});
}

Document ensure(const Document& doc, py::handle edge, bool prefix) {
    auto node = part_node(edge);
    return post(doc, Post{prefix ? "ensure_prefix" : "ensure_suffix", {node},
        [node, prefix](Session& session, const Bytes& input) {
            auto value = serialize_part(session.eval(node));
            const auto& bytes = value.data();
            auto& data = input.data();
            py::gil_scoped_release release;
            if (prefix)
                return data.starts_with(bytes) ? input : Bytes{std::string(bytes).append(data)};
            return data.ends_with(bytes) ? input : Bytes{std::string(data).append(bytes)};
        }});
}

Document pipe(const Document& doc, Operation operation, py::tuple args, py::dict kwargs) {
    args = py::reinterpret_borrow<py::tuple>(snapshot(args));
    kwargs = py::reinterpret_borrow<py::dict>(snapshot(kwargs));
    auto deps = dependencies(args);
    auto more = dependencies(kwargs);
    deps.insert(deps.end(), more.begin(), more.end());
    auto name = operation.name;
    const auto pure = operation.pure;
    return post(doc, Post{std::move(name), std::move(deps),
        [operation = std::move(operation), args = std::move(args), kwargs = std::move(kwargs)]
        (Session& session, const Bytes& input) {
            auto output = invoke_operation(session, operation, Value{input}, args, kwargs);
            if (auto* bytes = std::get_if<Bytes>(&output))
                return std::move(*bytes);
            throw Error(ErrorCode::operation, "Document.pipe must return bytes");
        }, pure});
}

Document observe(const Document& doc, py::object fn, std::optional<std::string> message) {
    if (!PyCallable_Check(fn.ptr()))
        throw Error(ErrorCode::type, "document callback must be callable");
    const auto name = message ? "require" : "tap";
    return post(doc, Post{name, {}, [fn = std::move(fn), message = std::move(message), name]
        (Session& session, const Bytes& input) {
            auto result = session.call(fn, py::make_tuple(to_python(Value{input})), py::dict(), name);
            if (message && !truth(result))
                throw Error(ErrorCode::operation, *message);
            return input;
        }, false});
}

Document budget(const Document& doc, Budget constraints) {
    auto current = std::make_shared<Layout>(layout(doc));
    if (!current->budget) {
        current->budget = constraints;
        return make_document(std::move(current));
    }
    auto tighten = [](auto& old, const auto& next) {
        if (next && (!old || *next < *old))
            old = next;
    };
    tighten(current->budget->bytes, constraints.bytes);
    tighten(current->budget->characters, constraints.characters);
    tighten(current->budget->tokens, constraints.tokens);
    return make_document(std::move(current));
}

} // namespace

void bind_document(py::module_& module) {
    using namespace py::literals;
    py::class_<Document, Source>(module, "Document")
        .def(py::init(&create), "format"_a = "text", "separator"_a = py::none())
        .def("append", [](const Document& doc, py::args parts) { return append(doc, parts); })
        .def("extend", [](const Document& doc, py::iterable parts) { return append(doc, parts); }, "parts"_a)
        .def("prepend", [](const Document& doc, py::args parts) { return append(doc, parts, true); })
        .def("insert", &insert, "index"_a, "part"_a)
        .def("remove", &remove, "index"_a)
        .def("replace_part", &replace_part, "index"_a, "part"_a)
        .def("slice_parts", &slice_parts, "start"_a = py::none(), "stop"_a = py::none())
        .def("concat", &concat, "other"_a, "separator"_a = py::none())
        .def("join", &join, "separator"_a = py::none())
        .def("map_parts", [](const Document& doc, py::object fn) {
            return part_step(doc, std::move(fn), false);
        }, "fn"_a)
        .def("filter_parts", [](const Document& doc, py::object pred) {
            return part_step(doc, std::move(pred), true);
        }, "pred"_a)
        .def("__add__", [](const Document& doc, py::handle part) {
            return append_one(doc, part_node(part));
        }, py::is_operator())
        .def("squish", &squish, "mode"_a = "auto")
        .def("replace", &replace, "old"_a, "new"_a, "count"_a = -1)
        .def("strip", [](const Document& doc) {
            const bool utf8 = layout(doc).format != Format::raw;
            return native_post(doc, "strip", [utf8](const Bytes& input) {
                if (utf8)
                    require_utf8(input.data(), ErrorCode::decode);
                return Bytes{std::string(strings::strip(input.data(), strings::Side::both, utf8))};
            });
        })
        .def("indent", [](const Document& doc, std::size_t spaces) {
            const bool utf8 = layout(doc).format != Format::raw;
            return native_post(doc, "indent", [spaces, utf8](const Bytes& input) {
                if (utf8)
                    require_utf8(input.data(), ErrorCode::decode);
                return Bytes{strings::indent(input.data(), spaces, utf8)};
            });
        }, "n"_a)
        .def("dedent", [](const Document& doc) {
            const bool utf8 = layout(doc).format != Format::raw;
            return native_post(doc, "dedent", [utf8](const Bytes& input) {
                return Bytes{strings::dedent(input.data(), utf8)};
            });
        })
        .def("normalize_newlines", [](const Document& doc, std::string style) {
            const auto newline = strings::newline_for(style);
            return native_post(doc, "newline.normalize", [newline](const Bytes& input) {
                return Bytes{strings::normalize_newlines(input.data(), newline)};
            });
        }, "style"_a = "lf")
        .def("wrap", &wrap, "prefix"_a, "suffix"_a)
        .def("ensure_prefix", [](const Document& doc, py::handle prefix) {
            return ensure(doc, prefix, true);
        }, "prefix"_a)
        .def("ensure_suffix", [](const Document& doc, py::handle suffix) {
            return ensure(doc, suffix, false);
        }, "suffix"_a)
        .def("pipe", [](const Document& doc, py::handle callable, py::args args, py::kwargs kwargs) {
            return pipe(doc, resolve_operation(Kind::document, callable), std::move(args), std::move(kwargs));
        }, "op"_a)
        .def("op", [](const Document& doc, py::str name, py::args args, py::kwargs kwargs) {
            return pipe(doc, resolve_operation(Kind::document, name), std::move(args), std::move(kwargs));
        }, "name"_a)
        .def("__getattr__", [](const Document& doc, py::str name) {
            const auto key = name.cast<std::string>();
            if (key.starts_with('_') || key.contains(':'))
                throw py::attribute_error(key);
            auto operation = resolve_operation(Kind::document, name);
            return py::cpp_function([doc, operation = std::move(operation)](py::args args, py::kwargs kwargs) {
                return pipe(doc, operation, std::move(args), std::move(kwargs));
            });
        })
        .def("require", [](const Document& doc, py::object predicate, std::string message) {
            return observe(doc, std::move(predicate), std::move(message));
        }, "predicate"_a, "message"_a = "requirement failed")
        .def("tap", [](const Document& doc, py::object fn) {
            return observe(doc, std::move(fn), std::nullopt);
        }, "fn"_a)
        .def("budget", &budget, "budget"_a);

    register_wrapper(Kind::document, [](NodePtr node) { return py::cast(Document{std::move(node)}); });
}

} // namespace syntwine
