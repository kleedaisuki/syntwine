#include "core.hpp"

#include "budget.hpp"
#include "evaluation_stop.hpp"
#include "markdown.hpp"
#include "telemetry.hpp"
#include "writer.hpp"

#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <system_error>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <utility>

namespace syntwine {
using namespace pybind11::literals;
using namespace pybind11::literals;

namespace {

struct Extension {
    py::object type, parse, serialize, validate;
};

struct ExtensionNode : NodeMetadata {
    explicit ExtensionNode(std::shared_ptr<const Extension> definition) : definition(std::move(definition)) {}
    std::shared_ptr<const Extension> definition;
};

struct Registry {
    std::map<std::tuple<PyObject*, std::string, std::string>, Operation> mounts;
    std::unordered_map<PyObject*, std::shared_ptr<const Extension>> extensions;
};

class Generated {
public:
    Value value;
    Bytes bytes;
    Usage usage;
    std::vector<std::string> sources;
    py::object trace;
};

thread_local Session* active_session = nullptr;

py::module_ native_module()
{
    return py::module_::import("syntwine._syntwine_native");
}

Registry& registry()
{
    return *static_cast<Registry*>(py::cast<py::capsule>(native_module().attr("_registry")).get_pointer());
}

py::object escaped_unicode(std::string_view bytes)
{
    auto text = py::reinterpret_steal<py::object>(
        PyUnicode_DecodeUTF8(bytes.data(), static_cast<Py_ssize_t>(bytes.size()), "surrogateescape"));
    if (!text)
        throw py::error_already_set();
    return text;
}

py::list source_paths(const std::vector<std::string>& paths)
{
    py::list result;
    for (const auto& path : paths)
        result.append(escaped_unicode(path));
    return result;
}

std::string path_bytes(const std::filesystem::path& path)
{
    auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::string path_display(std::string path)
{
    if (utf8_characters(path))
        return path;
    auto raw = py::reinterpret_steal<py::bytes>(
        PyUnicode_AsEncodedString(escaped_unicode(path).ptr(), "utf-8", "backslashreplace"));
    if (!raw)
        throw py::error_already_set();
    return py::cast<std::string>(raw);
}

NodePtr file_node(std::filesystem::path path)
{
    auto raw = path_bytes(path);
    auto source_id = path_display(raw);
    return make_node("source.read", Kind::literal, {},
                     [path = std::move(path), source_id, raw = std::move(raw)](Session& session) {
        session.record_source(raw);
        std::string content;
        {
            py::gil_scoped_release release;
            std::ifstream stream(path, std::ios::binary);
            auto fail = [&](std::string_view action, int error) {
                auto detail = error ? std::error_code(error, std::generic_category()).message() : "I/O error";
                throw Error(ErrorCode::source, "cannot " + std::string(action) +
                            " source " + source_id + ": " + detail, source_id);
            };
            if (!stream)
                fail("open", errno);
            std::error_code ignored;
            auto length = std::filesystem::file_size(path, ignored);
            if (!ignored && length <= content.max_size())
                content.reserve(static_cast<std::size_t>(length));
            char buffer[16384];
            while (stream) {
                stream.read(buffer, static_cast<std::streamsize>(sizeof buffer));
                content.append(buffer, static_cast<std::size_t>(stream.gcount()));
            }
            if (stream.bad() || !stream.eof())
                fail("read", errno);
        }
        return Bytes(std::move(content));
    }, true, {}, source_id, "source.read");
}

NodePtr cast_node(const Source& source, Kind kind, std::string encoding = "utf-8")
{
    std::string stage = kind == Kind::text ? "source.decode" : "parse." + std::string(kind_name(kind));
    if (kind == Kind::literal)
        stage = "output.serialize";
    return make_node("as_" + std::string(kind_name(kind)), kind, {source.node},
        [dep = source.node, kind, encoding = std::move(encoding), stage = std::move(stage)](Session& session) -> Value {
            auto value = session.eval(dep);
            if (kind == Kind::literal) {
                if (std::holds_alternative<Bytes>(value))
                    return value;
                if (auto text = std::get_if<Utf8>(&value))
                    return Bytes(*text);
            }
            std::string materialized;
            const auto bytes = [&]() -> std::string_view {
                if (const auto* content = std::get_if<Bytes>(&value))
                    return content->data();
                if (const auto* content = std::get_if<Utf8>(&value))
                    return content->data();
                materialized = serialize(value);
                return materialized;
            }();
            const auto* node = session.active();
            telemetry::Span span(session.trace(), stage, telemetry::Level::operations, node->id,
                                 node->source_id, node->name);
            span.input_bytes(bytes.size());
            try {
                auto result = kind == Kind::text && encoding != "utf-8" && encoding != "UTF-8"
                    ? from_python(py::bytes(bytes).attr("decode")(encoding, "strict"), Kind::text)
                    : parse(kind, bytes);
                span.output_bytes(size_bytes(result));
                return result;
            } catch (const py::error_already_set& cause) {
                Error error(ErrorCode::decode, cause.what());
                error.cause = cause.value();
                span.fail("DecodeError");
                throw error;
            } catch (const Error& error) {
                span.fail(kind_name_for_error(error.code));
                throw;
            }
        }, true, {}, source.node->source_id);
}

Kind nominal_kind(py::handle type)
{
    if (type.is(py::type::of<Literal>()) || type.ptr() == reinterpret_cast<PyObject*>(&PyBytes_Type))
        return Kind::literal;
    if (type.is(py::type::of<Text>()) || type.ptr() == reinterpret_cast<PyObject*>(&PyUnicode_Type))
        return Kind::text;
    if (type.is(py::type::of<MD>())) return Kind::md;
    if (type.is(py::type::of<TOML>())) return Kind::toml;
    if (type.is(py::type::of<JSON>())) return Kind::json;
    if (type.is(py::type::of<XML>())) return Kind::xml;
    if (type.is(native_module().attr("Document"))) return Kind::document;
    if (registry().extensions.contains(type.ptr())) return Kind::extension;
    return Kind::any;
}

py::object nominal_type(Kind kind)
{
    switch (kind) {
    case Kind::literal: return py::type::of<Literal>();
    case Kind::text: return py::type::of<Text>();
    case Kind::md: return py::type::of<MD>();
    case Kind::toml: return py::type::of<TOML>();
    case Kind::json: return py::type::of<JSON>();
    case Kind::xml: return py::type::of<XML>();
    case Kind::document: return native_module().attr("Document");
    default: return py::type::of<Source>();
    }
}

bool is_instance(py::handle value, py::handle type)
{
    int result = PyObject_IsInstance(value.ptr(), type.ptr());
    if (result < 0)
        throw py::error_already_set();
    return result == 1;
}

bool source_type(py::handle type)
{
    if (!PyType_Check(type.ptr()))
        return false;
    return PyType_IsSubtype(reinterpret_cast<PyTypeObject*>(type.ptr()),
                            reinterpret_cast<PyTypeObject*>(py::type::of<Source>().ptr())) == 1;
}

bool kind_accepts(Kind declared, Kind actual)
{
    return declared == Kind::any || actual == Kind::any || declared == actual ||
           (declared == Kind::literal && actual == Kind::document);
}

std::shared_ptr<const Extension> get_extension(py::handle cls)
{
    auto& formats = registry().extensions;
    auto item = formats.find(cls.ptr());
    if (item == formats.end())
        throw Error(ErrorCode::type, "format class has not been registered");
    return item->second;
}

py::object wrap_extension(NodePtr node)
{
    auto ext = std::dynamic_pointer_cast<const ExtensionNode>(node->metadata);
    if (!ext)
        return py::cast(Source(std::move(node)));
    auto cls = ext->definition->type;
    auto result = cls.attr("__new__")(cls);
    py::type::of<Source>().attr("__init__")(result, Source(std::move(node)));
    return result;
}

py::object extension_call(const py::object& callback, const py::tuple& args,
                          std::string_view name, ErrorCode code)
{
    try {
        if (active_session)
            return active_session->call(callback, args, py::dict(), name);
        return callback(*args);
    } catch (Error& failure) {
        failure.code = code;
        throw;
    }
}

class ExtensionValue final : public FormatValue {
public:
    ExtensionValue(std::shared_ptr<const Extension> definition, py::object value)
        : definition_(std::move(definition)), value_(std::move(value)) {}
    Kind kind() const noexcept override { return Kind::extension; }
    py::handle type() const { return definition_->type; }
    std::string serialize() const override
    {
        auto bytes = extension_call(definition_->serialize, py::make_tuple(value_), "format.serialize",
                                    ErrorCode::serialization);
        if (!PyBytes_Check(bytes.ptr()))
            throw Error(ErrorCode::serialization, "format serializer must return bytes");
        return py::cast<std::string>(bytes);
    }
    py::object to_python() const override { return value_; }

private:
    std::shared_ptr<const Extension> definition_;
    py::object value_;
};

Value extension_value(const std::shared_ptr<const Extension>& ext, py::object value)
{
    if (py::isinstance<Source>(value))
        throw Error(ErrorCode::type, "extension parsers and operations must return ordinary values");
    if (!ext->validate.is_none()) {
        auto valid = extension_call(ext->validate, py::make_tuple(value), "format.validate", ErrorCode::parse);
        if (!valid.is_none() && !PyObject_IsTrue(valid.ptr()))
            throw Error(ErrorCode::parse, "format validation failed");
        if (PyErr_Occurred())
            throw py::error_already_set();
    }
    return std::make_shared<const ExtensionValue>(ext, std::move(value));
}

py::object as_format(const Source& source, py::handle cls)
{
    auto ext = get_extension(cls);
    return wrap_source(make_node("as_format", Kind::extension, {source.node},
        [dep = source.node, ext](Session& session) {
            auto payload = serialize(session.eval(dep));
            auto val = extension_call(ext->parse, py::make_tuple(py::bytes(payload)), "format.parse",
                                      ErrorCode::parse);
            return extension_value(ext, std::move(val));
        }, true, std::make_shared<const ExtensionNode>(ext), source.node->source_id));
}

py::object source_from_value(py::handle cls, py::handle input)
{
    auto captured = snapshot(input);
    if (!cls.is(py::type::of<Source>())) {
        auto ext = get_extension(cls);
        return wrap_source(make_node("format.value", Kind::extension, dependencies(captured),
            [captured, ext](Session& session) {
                return extension_value(ext, to_python(session.resolve(captured)));
            }, true, std::make_shared<const ExtensionNode>(ext)));
    }
    return wrap_source(make_node("source.value", Kind::any, dependencies(captured),
        [captured](Session& session) { return session.resolve(captured); }));
}

Operation lookup_operation(Kind kind, std::string name, const Node* source = nullptr)
{
    std::string scope = "global";
    if (const auto i = name.find(':'); i != std::string::npos) {
        scope = name.substr(0, i);
        name.erase(0, i + 1);
    }
    auto& mounts = registry().mounts;
    auto lookup = [&](py::handle type) -> const Operation* {
        auto it = mounts.find({type.ptr(), scope, name});
        return it == mounts.end() ? nullptr : &it->second;
    };
    if (source && kind == Kind::extension) {
        auto ext = std::dynamic_pointer_cast<const ExtensionNode>(source->metadata);
        if (ext) {
            if (const auto* op = lookup(ext->definition->type))
                return *op;
        }
    }
    if (const auto* op = lookup(nominal_type(kind)))
        return *op;
    if (kind == Kind::literal || kind == Kind::document) {
        if (const auto* op = lookup(py::handle(reinterpret_cast<PyObject*>(&PyBytes_Type))))
            return *op;
    }
    if (kind == Kind::text) {
        if (const auto* op = lookup(py::handle(reinterpret_cast<PyObject*>(&PyUnicode_Type))))
            return *op;
    }
    if (const auto* op = lookup(py::type::of<Source>()))
        return *op;
    throw py::attribute_error("no mounted operation: " + name);
}

Operation declare_operation(py::object function, py::object input, py::object output, bool pure,
                            std::string version, py::object name, py::object configuration)
{
    if (!PyCallable_Check(function.ptr()))
        throw Error(ErrorCode::type, "operation must be callable");
    auto inferred = py::hasattr(function, "__name__") ? py::str(function.attr("__name__")) : py::str("python");
    auto label = py::cast<std::string>(name.is_none() ? inferred : py::str(name));
    return Operation{std::move(function), nominal_kind(input), nominal_kind(output), std::move(label),
        std::move(version), pure, std::move(input), std::move(output), std::move(configuration)};
}

py::object apply(const Source& source, py::handle op, const py::tuple& args, const py::dict& kwargs)
{
    auto operation = resolve_operation(source.node->kind, op, source.node.get());
    auto captured_args = py::reinterpret_borrow<py::tuple>(snapshot(args));
    auto captured_kwargs = py::reinterpret_borrow<py::dict>(snapshot(kwargs));
    auto deps = std::vector<NodePtr>{source.node};
    auto positional = dependencies(captured_args);
    auto keyword = dependencies(captured_kwargs);
    deps.insert(deps.end(), positional.begin(), positional.end());
    deps.insert(deps.end(), keyword.begin(), keyword.end());

    auto name = operation.name;
    auto output = operation.output;
    auto pure = operation.pure;
    auto metadata = output == Kind::extension
        ? std::make_shared<const ExtensionNode>(get_extension(operation.output_type))
        : std::shared_ptr<const ExtensionNode>{};
    if (output == Kind::document)
        output = Kind::literal; // The operation outputs bytes, not a Document Parts recipe.
    auto node = make_node(std::move(name), output, std::move(deps),
        [parent = source.node, op = std::move(operation), captured_args, captured_kwargs](Session& session) {
            return invoke_operation(session, op, session.eval(parent), captured_args, captured_kwargs);
        }, pure, std::move(metadata), source.node->source_id);
    return wrap_source(std::move(node));
}

void require_callable(py::handle fn)
{
    if (!PyCallable_Check(fn.ptr()))
        throw Error(ErrorCode::type, "expected a callable");
}

py::object transform(const Source& source, py::object fn, std::string name, bool pure)
{
    require_callable(fn);
    return wrap_source(make_node(name, Kind::any, {source.node},
        [parent = source.node, fn, name](Session& session) -> Value {
            auto result = session.call(fn, py::make_tuple(to_python(session.eval(parent))), py::dict());
            const bool is_source = py::isinstance<Source>(result);
            if (name == "dynamic.bind") {
                if (!is_source)
                    throw Error(ErrorCode::operation, "bind callback must return a Source");
                return session.eval(py::cast<const Source&>(result).node);
            }
            if (is_source)
                throw Error(ErrorCode::operation, "map callback must return an ordinary value; use bind for Source");
            return from_python(result);
        }, pure, {}, source.node->source_id, name == "dynamic.bind" ? name : ""));
}

py::object observe(const Source& source, py::object fn, std::optional<std::string> failure)
{
    require_callable(fn);
    return wrap_source(make_node(failure ? "require" : "tap", source.node->kind, {source.node},
        [parent = source.node, fn, failure](Session& session) -> Value {
            auto value = session.eval(parent);
            auto result = session.call(fn, py::make_tuple(to_python(value)), py::dict());
            if (failure) {
                if (py::isinstance<Source>(result))
                    throw Error(ErrorCode::operation, "require predicate must return an ordinary value");
                const auto passed = PyObject_IsTrue(result.ptr());
                if (passed < 0)
                    throw py::error_already_set();
                if (!passed)
                    throw Error(ErrorCode::operation, *failure);
            }
            return value;
        }, false, source.node->metadata, source.node->source_id));
}

py::dict inspect(const Source& source)
{
    py::list nodes;
    std::vector<NodePtr> pending{source.node};
    std::unordered_set<std::uint64_t> seen;
    while (!pending.empty()) {
        auto node = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(node->id).second)
            continue;
        auto declared = node->dependencies;
        if (node->metadata)
            node->metadata->append_declared_dependencies(declared);
        py::list deps;
        for (const auto& child : declared)
            deps.append(child->id);
        pending.insert(pending.end(), declared.rbegin(), declared.rend());
        nodes.append(py::dict("node_id"_a=node->id, "source_id"_a=node->source_id,
            "operation"_a=node->name, "kind"_a=kind_name(node->kind), "dependencies"_a=deps, "pure"_a=node->pure));
    }
    return py::dict("root"_a=source.node->id, "nodes"_a=nodes);
}

py::object trace_view(telemetry::Trace& trace) noexcept
{
    try {
        return trace.view();
    } catch (...) {
        // Diagnostic projection cannot replace the build result.
        PyErr_Clear();
        return py::none();
    }
}

void finish_failure(telemetry::Trace* trace, Error& error) noexcept
{
    if (!trace)
        return;
    trace->finish(kind_name_for_error(error.code));
    error.trace = trace_view(*trace);
}

Generated generate(const Source& source, py::object context, const std::optional<Budget>& budget,
                   py::object tokenizer, const std::optional<BuildOptions>& options,
                   py::object config, const std::filesystem::path* write_to = nullptr)
{
    if (active_session)
        throw Error(ErrorCode::operation, "cannot generate inside an evaluation; return a Source with bind instead");
    auto trace = telemetry::start(config, source.node->id, source.node->source_id, source.node->name);
    if (context.is_none()) {
        context = py::reinterpret_steal<py::object>(PyDictProxy_New(py::dict().ptr()));
        if (!context)
            throw py::error_already_set();
    }
    Session session(options.value_or(BuildOptions{}), std::move(context), std::move(tokenizer), trace.get());
    struct ActiveSession {
        explicit ActiveSession(Session& session) { active_session = &session; }
        ~ActiveSession() { active_session = nullptr; }
    } active(session);

    using telemetry::Level;
    using telemetry::Span;
    const auto& node = *source.node;
    auto stage = [&](std::string_view name) {
        return Span(trace.get(), name, Level::stages, node.id, node.source_id, node.name);
    };
    try {
        Value value;
        {
            auto span = stage("graph.resolve");
            try {
                value = session.eval(source.node);
            } catch (EvaluationStop& limit) {
                span.fail("EvaluationLimit");
                throw std::move(limit.error);
            }
        }
        auto bytes = [&] {
            auto span = stage("output.serialize");
            auto result = [&] {
                if (const auto* content = std::get_if<Bytes>(&value))
                    return *content;
                if (const auto* content = std::get_if<Utf8>(&value))
                    return Bytes(*content);
                return Bytes(serialize(value));
            }();
            span.output_bytes(result.data().size());
            return result;
        }();
        const Usage usage = session.validate_budget(bytes, budget.value_or(Budget{}), true);
        if (write_to) {
            auto span = stage("write");
            try {
                py::gil_scoped_release release;
                atomic_write(*write_to, bytes.data());
            } catch (const std::exception& cause) {
                span.fail("SourceError");
                throw Error(ErrorCode::source, cause.what(), path_display(path_bytes(*write_to)));
            }
        }
        py::object view = py::none();
        if (trace) {
            trace->finish({}, bytes.data());
            view = trace_view(*trace);
        }
        return {std::move(value), std::move(bytes), usage, session.sources(), std::move(view)};
    } catch (Error& error) {
        session.annotate(error, source.node);
        finish_failure(trace.get(), error);
        throw;
    } catch (const py::error_already_set& cause) {
        Error error(ErrorCode::operation, cause.what());
        error.cause = cause.value();
        session.annotate(error, source.node);
        finish_failure(trace.get(), error);
        throw error;
    } catch (const std::exception& cause) {
        Error error(ErrorCode::serialization, cause.what());
        session.annotate(error, source.node);
        finish_failure(trace.get(), error);
        throw error;
    }
}

} // namespace

Operation resolve_operation(Kind input, py::handle callable_or_name, const Node* source)
{
    Operation op;
    if (PyUnicode_Check(callable_or_name.ptr())) {
        op = lookup_operation(input, py::cast<std::string>(callable_or_name), source);
    } else if (py::isinstance<Operation>(callable_or_name)) {
        op = py::cast<const Operation&>(callable_or_name);
        if (op.input == Kind::any && source_type(op.input_type))
            op.input = nominal_kind(op.input_type);
        if (op.output == Kind::any && source_type(op.output_type))
            op.output = nominal_kind(op.output_type);
    } else {
        auto any = py::reinterpret_borrow<py::object>(reinterpret_cast<PyObject*>(&PyBaseObject_Type));
        op = declare_operation(py::reinterpret_borrow<py::object>(callable_or_name), any, any,
                               true, "1", py::none(), py::none());
    }
    if (!kind_accepts(op.input, input))
        throw Error(ErrorCode::type, "invalid input Source type for operation " + op.name);
    return op;
}

Value invoke_operation(Session& session, const Operation& op, const Value& value,
                       const py::tuple& args, const py::dict& kwargs)
{
    auto py_value = to_python(value);
    const bool valid = [&] {
        if (!source_type(op.input_type))
            return is_instance(py_value, op.input_type);
        if (op.input == Kind::any)
            return true;
        if (op.input == Kind::literal || op.input == Kind::document)
            return std::holds_alternative<Bytes>(value);
        if (op.input == Kind::text)
            return std::holds_alternative<Utf8>(value);
        const auto* format = std::get_if<std::shared_ptr<const FormatValue>>(&value);
        if (!format || (*format)->kind() != op.input)
            return false;
        if (op.input != Kind::extension)
            return true;
        auto ext = std::dynamic_pointer_cast<const ExtensionValue>(*format);
        return ext && ext->type().is(op.input_type);
    }();
    if (!valid)
        throw Error(ErrorCode::type, "invalid input value for operation " + op.name);

    auto resolved_args = py::reinterpret_borrow<py::tuple>(to_python(session.resolve(args)));
    auto resolved_kwargs = py::reinterpret_borrow<py::dict>(to_python(session.resolve(kwargs)));
    py::tuple positional(py::len(resolved_args) + 1);
    positional[0] = std::move(py_value);
    for (std::size_t i = 0; i < py::len(resolved_args); ++i)
        positional[i + 1] = resolved_args[i];
    auto output = session.call(op.function, positional, resolved_kwargs, op.name);
    if (py::isinstance<Source>(output))
        throw Error(ErrorCode::operation, "pipe callback must return an ordinary value; use bind for Source");
    if (op.output == Kind::extension)
        return extension_value(get_extension(op.output_type), std::move(output));
    if (!source_type(op.output_type) && !is_instance(output, op.output_type))
        throw Error(ErrorCode::operation, "operation " + op.name + " returned the wrong type");
    try {
        return from_python(output, op.output);
    } catch (const Error& error) {
        throw Error(ErrorCode::operation, "operation " + op.name + ": " + error.what(), error.location);
    }
}

} // namespace syntwine

namespace syntwine {

namespace {

py::dict diagnostic(const NodeDiagnostic& item)
{
    return py::dict("node_id"_a=item.node_id, "source_id"_a=item.source_id, "operation"_a=item.operation);
}

void translate_error(std::exception_ptr ptr)
{
    if (!ptr)
        return;
    try {
        std::rethrow_exception(ptr);
    } catch (const Error& error) {
        py::object type = native_module().attr(kind_name_for_error(error.code).data());
        auto object = type(escaped_unicode(error.what()));
        py::list path;
        for (const auto& item : error.dependency_path)
            path.append(diagnostic(item));
        object.attr("dependency_path") = std::move(path);
        const auto* origin = error.dependency_path.empty() ? nullptr : &error.dependency_path.back();
        object.attr("source_id") = origin ? py::cast(origin->source_id) : py::none();
        object.attr("operation") = origin ? py::cast(origin->operation) : py::none();
        object.attr("location") = error.location.empty() ? py::none() : py::cast(error.location);
        object.attr("trace_id") = error.trace_id.empty() ? py::none() : py::cast(error.trace_id);
        object.attr("trace") = error.trace ? error.trace : py::none();
        object.attr("cause") = error.cause ? error.cause : py::none();
        if (error.cause)
            object.attr("__cause__") = error.cause;
        if (error.code == ErrorCode::budget) {
            object.attr("actual") = py::cast(error.actual);
            object.attr("budget") = py::cast(error.budget);
            object.attr("dimension") = py::str(error.dimension);
            object.attr("overage") = py::int_(error.actual.value_or(0) - error.budget.value_or(0));
            object.attr("sources") = source_paths(error.sources);
        }
        PyErr_SetObject(type.ptr(), object.ptr());
    }
}

void bind_errors(py::module_& module)
{
    auto create = [&](std::string_view name, PyObject* base) {
        auto full_name = std::string("syntwine.") + std::string(name);
        auto type = py::reinterpret_steal<py::object>(PyErr_NewException(full_name.c_str(), base, nullptr));
        if (!type)
            throw py::error_already_set();
        module.attr(std::string(name).c_str()) = type;
        return type;
    };
    auto base = create("SyntwineError", PyExc_Exception);
    for (auto code : std::array{ErrorCode::source, ErrorCode::decode, ErrorCode::encoding, ErrorCode::parse,
        ErrorCode::type, ErrorCode::cycle, ErrorCode::operation, ErrorCode::conflict, ErrorCode::budget,
        ErrorCode::serialization})
        create(kind_name_for_error(code), base.ptr());
    create(kind_name_for_error(ErrorCode::limit), module.attr("OperationError").ptr());
    py::register_local_exception_translator(translate_error);
}

struct Ops {};

void bind_operations(py::module_& module)
{
    auto holder = std::make_unique<Registry>();
    module.attr("_registry") = py::capsule(holder.get(), [](void* pointer) {
        delete static_cast<Registry*>(pointer);
    });
    holder.release();

    py::class_<Operation>(module, "Operation")
        .def_readonly("name", &Operation::name)
        .def_readonly("version", &Operation::version)
        .def_readonly("pure", &Operation::pure)
        .def_readonly("input", &Operation::input_type)
        .def_readonly("output", &Operation::output_type)
        .def_readonly("configuration", &Operation::configuration)
        .def("__call__", [](const Operation& self, py::args args, py::kwargs kwargs) {
            return self.function(*args, **kwargs);
        });

    auto any = py::reinterpret_borrow<py::object>(reinterpret_cast<PyObject*>(&PyBaseObject_Type));
    module.def("operation", [](py::object input, py::object output, bool pure, std::string version,
                              py::object name, py::object configuration) {
        if (!PyType_Check(input.ptr()) || !PyType_Check(output.ptr()))
            throw Error(ErrorCode::type, "operation input/output must be types");
        return py::cpp_function([=, configuration = snapshot(configuration)](py::object function) {
            return declare_operation(std::move(function), input, output, pure, version, name, configuration);
        });
    }, py::kw_only(), "input"_a=any, "output"_a=any, "pure"_a=true, "version"_a="1",
        "name"_a=py::none(), "configuration"_a=py::none());

    py::class_<Ops>(module, "_Operations")
        .def("mount", [](const Ops&, py::object input, std::string name, py::object op, bool overwrite, std::string scope) {
            if (!PyType_Check(input.ptr()))
                throw Error(ErrorCode::type, "mount input must be a type");
            if (name.empty() || scope.empty() || name.contains(':') || scope.contains(':'))
                throw Error(ErrorCode::type, "mount name and scope must be nonempty and cannot contain ':'");
            auto kind = nominal_kind(input);
            if (kind == Kind::any && !input.is(py::type::of<Source>()))
                throw Error(ErrorCode::type, "mount input must be a registered Source type, str, or bytes");
            auto mounted = resolve_operation(kind, op);
            if (mounted.input_type.is(py::handle(reinterpret_cast<PyObject*>(&PyBaseObject_Type))))
                mounted.input_type = input;
            mounted.input = kind;
            mounted.name = name;
            auto& mounts = registry().mounts;
            auto key = std::tuple{input.ptr(), scope, name};
            if (!overwrite && (mounts.contains(key) ||
                              (scope == "global" && py::hasattr(source_type(input) ? input : nominal_type(kind), name.c_str()))))
                throw Error(ErrorCode::conflict, "operation already exists: " + name);
            mounts.insert_or_assign(std::move(key), std::move(mounted));
        }, "input"_a, "name"_a, "op"_a, py::kw_only(), "overwrite"_a=false, "scope"_a="global")
        .def("register_format", [](const Ops&, py::object cls, py::object parse, py::object serialize,
                                    py::object validate) {
            auto& formats = registry().extensions;
            if (!source_type(cls))
                throw Error(ErrorCode::type, "custom format must be a Source subclass");
            if (nominal_kind(cls) != Kind::any || cls.is(py::type::of<Source>()))
                throw Error(ErrorCode::conflict, "source format already registered");
            require_callable(parse);
            require_callable(serialize);
            if (!validate.is_none())
                require_callable(validate);
            auto* key = cls.ptr();
            formats.emplace(key, std::make_shared<const Extension>(
                Extension{std::move(cls), std::move(parse), std::move(serialize), std::move(validate)}));
        }, "cls"_a, "parse"_a, "serialize"_a, "validate"_a=py::none());
    module.attr("ops") = py::cast(Ops{});
}

void bind_results(py::module_& module)
{
    py::class_<Generated>(module, "Generated")
        .def_property_readonly("bytes", [](const Generated& result) { return py::bytes(result.bytes.data()); })
        .def_property_readonly("text", [](const Generated& result) {
            if (!result.usage.characters)
                throw Error(ErrorCode::decode, "artifact is not valid UTF-8");
            return py::str(result.bytes.data());
        })
        .def_property_readonly("value", [](const Generated& result) { return to_python(result.value); })
        .def_readonly("usage", &Generated::usage)
        .def_property_readonly("sources", [](const Generated& result) { return source_paths(result.sources); })
        .def_readonly("trace", &Generated::trace);

    py::class_<BuildOptions>(module, "BuildOptions")
        .def(py::init([](const std::string& cache, std::size_t max_nodes, std::size_t max_depth) {
            if (cache != "session" && cache != "none")
                throw py::value_error("cache must be 'session' or 'none'");
            if (!max_nodes || !max_depth)
                throw py::value_error("max_nodes and max_depth must be positive");
            return BuildOptions{cache == "session" ? Cache::session : Cache::none, max_nodes, max_depth};
        }), py::kw_only(), "cache"_a="session", "max_nodes"_a=100000, "max_depth"_a=512)
        .def_property_readonly("cache", [](const BuildOptions& self) {
            return self.cache == Cache::session ? "session" : "none";
        })
        .def_readonly("max_nodes", &BuildOptions::max_nodes)
        .def_readonly("max_depth", &BuildOptions::max_depth);
}

void bind_source(py::module_& module)
{
    py::class_<Source> source(module, "Source");
    source
        .def(py::init([](std::filesystem::path path) { return Source(file_node(std::move(path))); }), "path"_a)
        .def(py::init<const Source&>())
        .def_static("path", [](std::filesystem::path path) { return Source(file_node(std::move(path))); }, "path"_a)
        .def_static("defer", [](py::object fn, bool pure) {
            require_callable(fn);
            return Source(make_node("dynamic.defer", Kind::any, {}, [fn](Session& session) {
                auto value = session.call(fn, py::make_tuple(session.context()), py::dict());
                if (py::isinstance<Source>(value))
                    return session.eval(py::cast<const Source&>(value).node);
                return from_python(value);
            }, pure, {}, {}, "dynamic.defer"));
        }, "fn"_a, py::kw_only(), "pure"_a=true)
        .def("as_literal", [](const Source& self) { return Literal(cast_node(self, Kind::literal)); })
        .def("as_text", [](const Source& self, std::string encoding) {
            return Text(cast_node(self, Kind::text, std::move(encoding)));
        }, "encoding"_a="utf-8")
        .def("as_md", [](const Source& self) { return MD(cast_node(self, Kind::md)); })
        .def("as_toml", [](const Source& self) { return TOML(cast_node(self, Kind::toml)); })
        .def("as_json", [](const Source& self) { return JSON(cast_node(self, Kind::json)); })
        .def("as_xml", [](const Source& self) { return XML(cast_node(self, Kind::xml)); })
        .def("as_format", as_format, "cls"_a)
        .def("codeblock", markdown_codeblock, "language"_a="")
        .def("pipe", [](const Source& self, py::object op, py::args args, py::kwargs kwargs) {
            return apply(self, op, args, kwargs);
        }, "op"_a)
        .def("op", [](const Source& self, py::str name, py::args args, py::kwargs kwargs) {
            return apply(self, name, args, kwargs);
        }, "name"_a)
        .def("__getattr__", [](const Source& self, const std::string& name) {
            if (name.starts_with('_') || name.contains(':'))
                throw py::attribute_error(name);
            auto op = lookup_operation(self.node->kind, name, self.node.get());
            return py::cpp_function([self, op = py::cast(std::move(op))](py::args args, py::kwargs kwargs) {
                return apply(self, op, args, kwargs);
            });
        })
        .def("map", [](const Source& self, py::object fn, bool pure) {
            return transform(self, std::move(fn), "map", pure);
        }, "fn"_a, py::kw_only(), "pure"_a=true)
        .def("bind", [](const Source& self, py::object fn, bool pure) {
            return transform(self, std::move(fn), "dynamic.bind", pure);
        }, "fn"_a, py::kw_only(), "pure"_a=true)
        .def("zip", [](const Source& self, const Source& other) {
            return Source(make_node("zip", Kind::any, {self.node, other.node}, [left = self.node, right = other.node](Session& s) {
                auto a = to_python(s.eval(left));
                auto b = to_python(s.eval(right));
                return py::make_tuple(std::move(a), std::move(b));
            }));
        }, "other"_a)
        .def("combine", [](const Source& self, py::args others, py::object fn) {
            require_callable(fn);
            std::vector<NodePtr> nodes{self.node};
            for (auto object : others) {
                if (!py::isinstance<Source>(object))
                    throw Error(ErrorCode::type, "combine dependencies must be Source objects");
                nodes.push_back(py::cast<const Source&>(object).node);
            }
            auto deps = nodes;
            return Source(make_node("combine", Kind::any, std::move(deps), [deps = std::move(nodes), fn](Session& s) {
                py::tuple values(deps.size());
                for (std::size_t i = 0; i < deps.size(); ++i)
                    values[i] = to_python(s.eval(deps[i]));
                auto result = s.call(fn, values, py::dict());
                if (py::isinstance<Source>(result))
                    throw Error(ErrorCode::operation, "combine callback must return an ordinary value");
                return from_python(result);
            }));
        }, "fn"_a)
        .def("require", [](const Source& self, py::object fn, std::string message) {
            return observe(self, std::move(fn), std::move(message));
        }, "predicate"_a, "message"_a="requirement failed")
        .def("tap", [](const Source& self, py::object fn) {
            return observe(self, std::move(fn), std::nullopt);
        }, "fn"_a)
        .def("inspect", inspect)
        .def("explain", inspect)
        .def("generate", [](const Source& self, py::object context, const std::optional<Budget>& budget,
                        py::object tokenizer, const std::optional<BuildOptions>& options, py::object config) {
            return generate(self, std::move(context), budget, std::move(tokenizer), options, std::move(config));
        }, py::kw_only(), "context"_a=py::none(), "budget"_a=py::none(), "tokenizer"_a=py::none(),
            "options"_a=py::none(), "telemetry"_a=py::none())
        .def("write", [](const Source& self, const std::filesystem::path& path, py::object context,
                         const std::optional<Budget>& budget, py::object tokenizer,
                         const std::optional<BuildOptions>& options, py::object config) {
            return generate(self, std::move(context), budget, std::move(tokenizer), options, std::move(config), &path);
        }, "path"_a, py::kw_only(), "context"_a=py::none(), "budget"_a=py::none(), "tokenizer"_a=py::none(),
            "options"_a=py::none(), "telemetry"_a=py::none());

    register_wrapper(Kind::extension, wrap_extension);
}

} // namespace

void bind_core(py::module_& module)
{
    bind_errors(module);
    bind_results(module);
    bind_source(module);
    bind_operations(module);
}

void bind_source_aliases(py::module_& module)
{
    // pybind11 resolves inherited descriptors while creating derived methods, so add
    // these descriptors only after the native formats have their own from_value methods.
    auto classmethod = py::module_::import("builtins").attr("classmethod");
    module.attr("Source").attr("from_value") = classmethod(
        py::cpp_function(source_from_value, "cls"_a, "value"_a));
    auto parameterize = classmethod(py::cpp_function([](py::object cls, py::object type) {
        return py::module_::import("types").attr("GenericAlias")(cls, type);
    }));
    for (auto name : {"Source", "Generated", "Operation"})
        module.attr(name).attr("__class_getitem__") = parameterize;
}

} // namespace syntwine
