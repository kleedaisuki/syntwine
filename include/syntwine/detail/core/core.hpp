#pragma once

#include <pybind11/pybind11.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace syntwine {

namespace py = pybind11;

namespace telemetry {
class Trace;
}

struct Budget;
struct Usage;

enum class Kind { any, literal, text, md, toml, json, xml, document, extension };
std::string_view kind_name(Kind kind);

template <class Tag>
class Buffer {
public:
    explicit Buffer(std::string data)
        : data_(std::make_shared<const std::string>(std::move(data))) {}

    template <class Other>
    explicit Buffer(const Buffer<Other>& buffer)
        : data_(buffer.data_) {}

    const std::string&
    data() const noexcept {
        return *data_;
    }

private:
    template <class>
    friend class Buffer;
    friend class Session;
    std::shared_ptr<const std::string> data_;
};
using Bytes = Buffer<struct BytesTag>;
using Utf8 = Buffer<struct Utf8Tag>;

struct FormatValue {
    virtual ~FormatValue() = default;
    virtual Kind kind() const noexcept = 0;
    virtual std::string serialize() const = 0;
    virtual py::object to_python() const = 0;
};

using Value =
    std::variant<std::monostate, Bytes, Utf8, std::shared_ptr<const FormatValue>, py::object>;

enum class ErrorCode {
    source,
    decode,
    encoding,
    parse,
    type,
    cycle,
    operation,
    conflict,
    budget,
    serialization,
    limit
};

std::string_view kind_name_for_error(ErrorCode code);

struct NodeDiagnostic {
    std::uint64_t node_id;
    std::string source_id, operation;
};

struct Error : std::runtime_error {
    Error(ErrorCode code, std::string message, std::string location = {});

    ErrorCode code;
    std::string location, trace_id;
    std::vector<NodeDiagnostic> dependency_path;
    py::object cause, trace;
    std::string dimension;
    std::optional<std::size_t> actual, budget;
    std::vector<std::string> sources;
};

enum class Cache { session, none };

struct BuildOptions {
    Cache cache = Cache::session;
    std::size_t max_nodes = 100000;
    std::size_t max_depth = 512;
};

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct NodeMetadata {
    virtual ~NodeMetadata() = default;

    virtual void
    append_declared_dependencies(std::vector<NodePtr>&) const {}
};
class Session;
using Compute = std::function<Value(Session&)>;

struct Node {
    std::uint64_t id;
    std::string name;
    Kind kind;
    std::vector<NodePtr> dependencies;
    Compute compute;
    bool pure;
    std::shared_ptr<const NodeMetadata> metadata;
    std::string source_id;
    std::string span;
    mutable const Node* next_free = nullptr;
};

NodePtr make_node(
    std::string name,
    Kind kind,
    std::vector<NodePtr> dependencies,
    Compute compute,
    bool pure = true,
    std::shared_ptr<const NodeMetadata> metadata = {},
    std::string source_id = {},
    std::string span = {}
);

struct Source {
    explicit Source(NodePtr value)
        : node(std::move(value)) {}

    NodePtr node;
};

struct Literal : Source {
    using Source::Source;
};

struct Text : Source {
    using Source::Source;
};

struct MD : Source {
    using Source::Source;
};

struct TOML : Source {
    using Source::Source;
};

struct JSON : Source {
    using Source::Source;
};

struct XML : Source {
    using Source::Source;
};

class Session {
public:
    Session(
        BuildOptions options,
        py::object context,
        py::object tokenizer,
        telemetry::Trace* trace
    );
    Value eval(const NodePtr& node);
    Value resolve(py::handle input);
    Usage validate_budget(std::string_view bytes, const Budget& budget, bool count_usage = false);
    Usage validate_budget(const Bytes& bytes, const Budget& budget, bool count_usage = false);
    py::object call(
        py::handle fn,
        const py::tuple& args,
        const py::dict& kwargs,
        std::string_view operation = {}
    );
    void annotate(Error& error, const NodePtr& extra = {}) const;
    void record_source(std::string_view source_id);

    telemetry::Trace*
    trace() const noexcept {
        return trace_;
    }

    const py::object&
    context() const noexcept {
        return context_;
    }

    const std::vector<std::string>&
    sources() const noexcept {
        return sources_;
    }

    const Node*
    active() const noexcept {
        return stack_.empty() ? nullptr : stack_.back().node.get();
    }

private:
    enum class State { unseen, visiting, done, failed };

    struct Record {
        State state = State::unseen;
        Value value;
        std::exception_ptr failure;
    };

    struct Frame {
        NodePtr node;
        bool pure;
        std::unique_ptr<std::unordered_set<std::uint64_t>> declared;
        std::size_t discovered = 0;
    };

    Value evaluate(const NodePtr& node, Record& record);
    void trace_dependency(const NodePtr& dependency);
    Usage validate(
        std::string_view bytes,
        const Budget& budget,
        bool count_usage,
        const std::shared_ptr<const std::string>& stable_bytes
    );

    BuildOptions options_;
    py::object context_, tokenizer_;
    telemetry::Trace* trace_;
    const Node* root_ = nullptr;
    std::vector<Frame> stack_;
    std::unordered_map<std::uint64_t, Record> records_;
    std::vector<std::string> sources_;
    std::unordered_set<std::string> source_ids_;
    std::shared_ptr<const std::string> last_token_bytes_;
    std::size_t last_token_count_ = 0;
};

using ParseFunction = std::function<Value(std::string_view)>;
using FromPythonFunction = std::function<Value(py::handle)>;
using Wrapper = std::function<py::object(NodePtr)>;

void register_format(Kind kind, ParseFunction parse, FromPythonFunction from_python);
void register_wrapper(Kind kind, Wrapper wrap);
Value parse(Kind kind, std::string_view bytes);
Value from_python(py::handle input, Kind kind = Kind::any);
py::object to_python(const Value& value);
std::string serialize(const Value& value);
std::optional<std::size_t> size_bytes(const Value& value);
py::object wrap_source(NodePtr source);
std::vector<NodePtr> dependencies(py::handle operand);
py::object snapshot(py::handle operand);

struct Operation {
    py::object function;
    Kind input = Kind::any, output = Kind::any;
    std::string name, version;
    bool pure = true;
    py::object input_type, output_type, configuration;
};

Operation resolve_operation(Kind input, py::handle callable_or_name, const Node* source = nullptr);
Value invoke_operation(
    Session& session,
    const Operation& op,
    const Value& value,
    const py::tuple& args,
    const py::dict& kwargs
);

void bind_core(py::module_& module);
void bind_source_aliases(py::module_& module);

} // namespace syntwine
