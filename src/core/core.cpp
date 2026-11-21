#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "syntwine/detail/core/core.hpp"

#include "syntwine/detail/core/budget.hpp"
#include "syntwine/detail/core/evaluation_stop.hpp"
#include "syntwine/detail/telemetry/telemetry.hpp"

#include <pybind11/stl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace syntwine {

using telemetry::Level;
using telemetry::Span;

Error::Error(ErrorCode code, std::string message, std::string location)
    : std::runtime_error(std::move(message)),
      code(code),
      location(std::move(location)) {}

std::string_view
kind_name(Kind kind) {
    static constexpr std::array
        names{"source", "literal", "text", "md", "toml", "json", "xml", "document", "extension"};
    return names[static_cast<std::size_t>(kind)];
}

std::string_view
kind_name_for_error(ErrorCode code) {
    static constexpr std::array names{
        "SourceError",
        "DecodeError",
        "EncodingError",
        "ParseError",
        "TypeMismatch",
        "CycleError",
        "OperationError",
        "OperationConflict",
        "BudgetExceeded",
        "SerializationError",
        "EvaluationLimit"
    };
    return names[static_cast<std::size_t>(code)];
}

namespace {

void
delete_node(const Node* node) noexcept {
    // Releasing the last root of an arbitrarily deep graph must not recurse through its edges.
    static thread_local const Node* pending = nullptr;
    static thread_local bool draining = false;
    node->next_free = pending;
    pending = node;
    if (draining)
        return;
    draining = true;
    while (pending) {
        auto* item = pending;
        pending = item->next_free;
        delete item;
    }
    draining = false;
}

struct StackRange {
    std::uintptr_t low = 0, high = 0;

    bool
    contains(std::uintptr_t address) const noexcept {
        return address >= low && address < high;
    }
};

// The live stack address is needed on Windows to resolve the reservation and
// to detect switches between stacks on the same native thread.
#if defined(_WIN32)
StackRange
stack_range(std::uintptr_t current) noexcept {
    ULONG_PTR low{}, high{};
    GetCurrentThreadStackLimits(&low, &high);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(current), &region, sizeof region)
        && region.State == MEM_COMMIT
        && region.Type == MEM_PRIVATE) {
        low = reinterpret_cast<ULONG_PTR>(region.AllocationBase);
        if (high <= current)
            high = reinterpret_cast<ULONG_PTR>(region.BaseAddress) + region.RegionSize;
    }
    ULONG guarantee = 0;
    SetThreadStackGuarantee(&guarantee);
    return {low + guarantee, high};
}

bool
stack_headroom() noexcept {
#if defined(_MSC_VER)
    auto current = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    auto current = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
    static thread_local StackRange range;
    if (!range.contains(current))
        range = stack_range(current);
    if (!range.contains(current))
        return false;
    // Reserve room for the MSVC exception dispatcher, pybind11 and callback frames.
    auto reserve = std::clamp(
        (range.high - range.low) / 3,
        std::uintptr_t{64 * 1024},
        std::uintptr_t{512 * 1024}
    );
    return current - range.low >= reserve;
}

#else
StackRange
stack_range() noexcept {
#if defined(__APPLE__)
    auto thread = pthread_self();
    auto high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
    return {high - pthread_get_stacksize_np(thread), high};
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr))
        return {};
    void* address{};
    std::size_t size{}, guard{};
    pthread_attr_getguardsize(&attr, &guard);
    int status = pthread_attr_getstack(&attr, &address, &size);
    pthread_attr_destroy(&attr);
    auto low = reinterpret_cast<std::uintptr_t>(address);
    return status ? StackRange{} : StackRange{low + guard, low + size};
#else
    return {};
#endif
}

bool
stack_headroom() noexcept {
    static thread_local const auto range = stack_range();
    const char marker = 0;
    auto current = reinterpret_cast<std::uintptr_t>(&marker);
    if (!range.contains(current))
        return true; // CPython's recursion API still checks platform-specific or alternate stacks.
    // C++ unwinding and callback wrappers can consume more than CPython's own reserve.
    auto reserve = std::clamp(
        (range.high - range.low) / 4,
        std::uintptr_t{16 * 1024},
        std::uintptr_t{128 * 1024}
    );
    return current - range.low >= reserve;
}
#endif

std::size_t
evaluation_depth_limit(std::size_t configured) noexcept {
#if defined(_WIN32)
    // MSVC's exception dispatcher must also unwind the suspended callbacks.
    // Free bytes alone do not bound that work on a deeply nested graph.
    return std::min(configured, std::size_t{256});
#else
    return configured;
#endif
}

struct RecursionGuard {
    explicit RecursionGuard(const char* where) {
        const auto has_room = stack_headroom();
        if (has_room && !Py_EnterRecursiveCall(where))
            return;
        Error error(ErrorCode::limit, "native stack limit exceeded");
        if (!has_room)
            PyErr_SetString(PyExc_RecursionError, where);
        error.cause = py::reinterpret_steal<py::object>(PyErr_GetRaisedException());
        throw error;
    }

    ~RecursionGuard() {
        Py_LeaveRecursiveCall();
    }

    RecursionGuard(const RecursionGuard&) = delete;
    RecursionGuard& operator=(const RecursionGuard&) = delete;
};

} // namespace

NodePtr
make_node(
    std::string name,
    Kind kind,
    std::vector<NodePtr> dependencies,
    Compute compute,
    bool pure,
    std::shared_ptr<const NodeMetadata> metadata,
    std::string source_id,
    std::string span
) {
    static std::atomic<std::uint64_t> next_id{1};
    auto id = next_id.fetch_add(1, std::memory_order_relaxed);
    if (source_id.empty())
        source_id = "node:" + std::to_string(id);
    if (span.empty()
        && (name.starts_with("literal.")
            || name.starts_with("text.")
            || name.starts_with("md.")
            || name.starts_with("json.")
            || name.starts_with("toml.")
            || name.starts_with("xml.")))
        span = "transform";
    return NodePtr(
        new Node{
            id,
            std::move(name),
            kind,
            std::move(dependencies),
            std::move(compute),
            pure,
            std::move(metadata),
            std::move(source_id),
            std::move(span)
        },
        delete_node
    );
}

namespace {
struct Format {
    ParseFunction parse;
    FromPythonFunction from_python;
    Wrapper wrap;
};

Format&
format(Kind kind) {
    static std::array<Format, static_cast<std::size_t>(Kind::extension) + 1> formats;
    return formats[static_cast<std::size_t>(kind)];
}

bool
is_container(py::handle object) {
    return PyDict_Check(object.ptr()) || PyList_Check(object.ptr()) || PyTuple_Check(object.ptr());
}

void
find_dependencies(
    py::handle object,
    std::vector<NodePtr>& result,
    std::unordered_set<PyObject*>& containers
) {
    if (py::isinstance<Source>(object)) {
        result.push_back(py::cast<const Source&>(object).node);
        return;
    }
    if (!is_container(object) || !containers.insert(object.ptr()).second)
        return;
    RecursionGuard guard(" while inspecting Syntwine operands");
    if (PyDict_Check(object.ptr())) {
        for (auto item : py::reinterpret_borrow<py::dict>(object))
            find_dependencies(item.second, result, containers);
        return;
    }
    for (auto item : py::reinterpret_borrow<py::iterable>(object))
        find_dependencies(item, result, containers);
}

template <class Leaf>
py::object
copy_tree(py::handle input, Leaf& leaf, std::unordered_set<PyObject*>& active) {
    if (!is_container(input))
        return leaf(input);
    RecursionGuard guard(" while resolving Syntwine operands");
    if (!active.insert(input.ptr()).second)
        throw Error(ErrorCode::type, "cyclic operand container");
    py::object result;
    if (PyDict_Check(input.ptr())) {
        py::dict copy;
        for (auto [key, value] : py::reinterpret_borrow<py::dict>(input))
            copy[key] = copy_tree(value, leaf, active);
        result = std::move(copy);
    } else {
        const auto seq = py::reinterpret_borrow<py::sequence>(input);
        const auto n = py::len(seq);
        if (PyTuple_Check(input.ptr())) {
            py::tuple copy(n);
            for (std::size_t i = 0; i < n; ++i)
                copy[i] = copy_tree(seq[i], leaf, active);
            result = std::move(copy);
        } else {
            py::list copy(n);
            for (std::size_t i = 0; i < n; ++i)
                copy[i] = copy_tree(seq[i], leaf, active);
            result = std::move(copy);
        }
    }
    active.erase(input.ptr());
    return result;
}

} // namespace

std::vector<NodePtr>
dependencies(py::handle operand) {
    std::vector<NodePtr> result;
    std::unordered_set<PyObject*> containers;
    find_dependencies(operand, result, containers);
    return result;
}

py::object
snapshot(py::handle operand) {
    std::unordered_set<PyObject*> active;
    auto leaf = [](py::handle value) { return py::reinterpret_borrow<py::object>(value); };
    return copy_tree(operand, leaf, active);
}

void
register_format(Kind kind, ParseFunction parse_fn, FromPythonFunction convert_fn) {
    auto& item = format(kind);
    item.parse = std::move(parse_fn);
    item.from_python = std::move(convert_fn);
}

void
register_wrapper(Kind kind, Wrapper wrap) {
    format(kind).wrap = std::move(wrap);
}

Value
parse(Kind kind, std::string_view data) {
    if (kind == Kind::literal || kind == Kind::document)
        return Bytes(std::string(data));
    if (kind == Kind::text) {
        if (!utf8_characters(data))
            throw Error(ErrorCode::decode, "invalid UTF-8");
        return Utf8(std::string(data));
    }
    if (const auto& fn = format(kind).parse)
        return fn(data);
    throw Error(ErrorCode::type, "no parser registered for " + std::string(kind_name(kind)));
}

Value
from_python(py::handle object, Kind kind) {
    if (PyBytes_Check(object.ptr())
        && (kind == Kind::any || kind == Kind::literal || kind == Kind::document))
        return Bytes(py::cast<std::string>(object));
    if (PyUnicode_Check(object.ptr()) && (kind == Kind::any || kind == Kind::text)) {
        Py_ssize_t length{};
        const char* data = PyUnicode_AsUTF8AndSize(object.ptr(), &length);
        if (!data) {
            py::error_already_set cause;
            Error error(ErrorCode::encoding, "text is not valid UTF-8");
            error.cause = cause.value();
            throw error;
        }
        return Utf8(std::string(data, static_cast<std::size_t>(length)));
    }
    if (kind == Kind::any) {
        if (object.is_none())
            return std::monostate{};
        return py::reinterpret_borrow<py::object>(object);
    }
    if (const auto& fn = format(kind).from_python)
        return fn(object);
    throw Error(ErrorCode::type, "expected " + std::string(kind_name(kind)) + " value");
}

py::object
to_python(const Value& value) {
    return std::visit(
        [](const auto& item) -> py::object {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return py::none();
            else if constexpr (std::is_same_v<T, Bytes>)
                return py::bytes(item.data());
            else if constexpr (std::is_same_v<T, Utf8>)
                return py::str(item.data());
            else if constexpr (std::is_same_v<T, std::shared_ptr<const FormatValue>>)
                return item->to_python();
            else
                return item;
        },
        value
    );
}

std::string
serialize(const Value& value) {
    if (const auto* bin = std::get_if<Bytes>(&value))
        return bin->data();
    if (const auto* text = std::get_if<Utf8>(&value))
        return text->data();
    if (const auto* fmt = std::get_if<std::shared_ptr<const FormatValue>>(&value))
        return (*fmt)->serialize();
    try {
        return serialize(from_python(to_python(value), Kind::json));
    } catch (const Error& cause) {
        throw Error(ErrorCode::serialization, cause.what(), cause.location);
    }
}

std::optional<std::size_t>
size_bytes(const Value& value) {
    if (const auto* bin = std::get_if<Bytes>(&value))
        return bin->data().size();
    if (const auto* text = std::get_if<Utf8>(&value))
        return text->data().size();
    return std::nullopt;
}

py::object
wrap_source(NodePtr node) {
    if (const auto& fn = format(node->kind).wrap)
        return fn(std::move(node));
    switch (node->kind) {
    case Kind::literal:
        return py::cast(Literal(std::move(node)));
    case Kind::text:
        return py::cast(Text(std::move(node)));
    case Kind::md:
        return py::cast(MD(std::move(node)));
    case Kind::toml:
        return py::cast(TOML(std::move(node)));
    case Kind::json:
        return py::cast(JSON(std::move(node)));
    case Kind::xml:
        return py::cast(XML(std::move(node)));
    default:
        return py::cast(Source(std::move(node)));
    }
}

Session::Session(
    BuildOptions options,
    py::object context,
    py::object tokenizer,
    telemetry::Trace* trace
)
    : options_(options),
      context_(std::move(context)),
      tokenizer_(std::move(tokenizer)),
      trace_(trace) {}

void
Session::record_source(std::string_view source_id) {
    auto [it, inserted] = source_ids_.emplace(source_id);
    if (inserted)
        sources_.push_back(*it);
}

void
Session::annotate(Error& error, const NodePtr& extra) const {
    if (trace_ && error.trace_id.empty())
        error.trace_id = trace_->id();
    if (error.dependency_path.empty()) {
        auto append = [&error](const NodePtr& node) {
            error.dependency_path.push_back({node->id, node->source_id, node->name});
        };
        for (const auto& frame : stack_)
            append(frame.node);
        if (extra)
            append(extra);
    }
    if (error.code == ErrorCode::budget && error.sources.empty()) {
        error.sources = sources_;
        if (error.sources.empty() && !error.dependency_path.empty())
            error.sources.push_back(error.dependency_path.back().source_id);
    }
}

void
Session::trace_dependency(const NodePtr& dependency) {
    if (stack_.empty() || !trace_->records_dependencies())
        return;
    auto& parent = stack_.back();
    const auto& declared = parent.node->dependencies;
    bool discovered;
    if (!parent.node->metadata && declared.size() <= 32) {
        discovered = std::ranges::none_of(declared, [&dependency](const NodePtr& node) {
            return node->id == dependency->id;
        });
    } else {
        if (!parent.declared) {
            parent.declared = std::make_unique<std::unordered_set<std::uint64_t>>();
            std::vector<NodePtr> lazy;
            if (parent.node->metadata)
                parent.node->metadata->append_declared_dependencies(lazy);
            parent.declared->reserve(declared.size() + lazy.size());
            for (const auto& node : declared)
                parent.declared->insert(node->id);
            for (const auto& node : lazy)
                parent.declared->insert(node->id);
        }
        discovered = !parent.declared->contains(dependency->id);
    }
    parent.discovered += discovered;
    trace_->dependency(parent.node->id, dependency->id, discovered);
}

Value
Session::eval(const NodePtr& node) {
    if (!root_)
        root_ = node.get();
    if (trace_)
        trace_dependency(node);
    auto [pos, inserted] = records_.try_emplace(node->id);
    auto& record = pos->second;
    if (record.state == State::visiting) {
        std::string message = "dependency cycle";
        for (const auto& frame : stack_)
            message += " -> " + frame.node->name + " (" + frame.node->source_id + ")";
        message += " -> " + node->name + " (" + node->source_id + ")";
        Error error(ErrorCode::cycle, std::move(message));
        annotate(error, node);
        throw error;
    }
    if (record.state == State::failed)
        std::rethrow_exception(record.failure);
    if (record.state == State::done) {
        if (trace_) {
            Span span(
                trace_,
                "node.evaluate",
                Level::operations,
                node->id,
                node->source_id,
                node->name
            );
            span.cache_hit(true);
            span.output_bytes(size_bytes(record.value));
        }
        return record.value;
    }
    if (stack_.size() >= evaluation_depth_limit(options_.max_depth)) {
        Error error(ErrorCode::limit, "evaluation depth limit exceeded");
        annotate(error, node);
        throw EvaluationStop{std::move(error)};
    }
    if (inserted && records_.size() > options_.max_nodes) {
        Error error(ErrorCode::limit, "evaluation node limit exceeded");
        annotate(error, node);
        throw EvaluationStop{std::move(error)};
    }
    try {
        RecursionGuard guard(" while evaluating the Syntwine graph");
        return evaluate(node, record);
    } catch (Error& error) {
        annotate(error, node);
        record.state = State::failed;
        if (error.code == ErrorCode::limit)
            throw EvaluationStop{std::move(error)};
        record.failure = std::current_exception();
        throw;
    } catch (const std::exception&) {
        record.state = State::failed;
        record.failure = std::current_exception();
        throw;
    }
}

namespace {
template <class F>
struct EvaluationFrame {
    F pop;

    ~EvaluationFrame() {
        pop();
    }
};
} // namespace

Value
Session::evaluate(const NodePtr& node, Record& record) {
    stack_.push_back({node, node->pure, {}});
    record.state = State::visiting;
    EvaluationFrame frame{[&] {
        if (record.state == State::visiting)
            record.state = State::failed;
        bool pure = stack_.back().pure;
        stack_.pop_back();
        if (!stack_.empty() && !pure)
            stack_.back().pure = false;
    }};
    Span span(trace_, "node.evaluate", Level::operations, node->id, node->source_id, node->name);
    try {
        Value result;
        if (node->span.empty()) {
            result = node->compute(*this);
        } else {
            Span detail(
                trace_,
                node->span,
                Level::operations,
                node->id,
                node->source_id,
                node->name
            );
            try {
                result = node->compute(*this);
                if (trace_)
                    detail.output_bytes(size_bytes(result));
            } catch (const Error& error) {
                detail.fail(kind_name_for_error(error.code));
                throw;
            }
        }
        if (trace_) {
            span.output_bytes(size_bytes(result));
            if (stack_.back().discovered)
                span.discovered_dependencies(stack_.back().discovered);
        }
        if (stack_.back().pure && options_.cache == Cache::session) {
            record.value = result;
            record.state = State::done;
        } else {
            record.state = State::unseen;
        }
        return result;
    } catch (Error& error) {
        annotate(error);
        span.fail(kind_name_for_error(error.code));
        throw;
    } catch (const py::error_already_set& cause) {
        Error error(ErrorCode::operation, "Python operation failed: " + std::string(cause.what()));
        error.cause = cause.value();
        annotate(error);
        span.fail("OperationError");
        throw error;
    } catch (const std::exception& cause) {
        Error error(ErrorCode::operation, cause.what());
        annotate(error);
        span.fail("OperationError");
        throw error;
    }
}

Value
Session::resolve(py::handle input) {
    if (py::isinstance<Source>(input))
        return eval(py::cast<const Source&>(input).node);
    if (!is_container(input))
        return from_python(input);
    std::unordered_set<PyObject*> active;
    auto leaf = [this](py::handle item) {
        if (py::isinstance<Source>(item))
            return to_python(eval(py::cast<const Source&>(item).node));
        return py::reinterpret_borrow<py::object>(item);
    };
    return copy_tree(input, leaf, active);
}

py::object
Session::call(
    py::handle fn,
    const py::tuple& args,
    const py::dict& kwargs,
    std::string_view operation
) {
    const auto* node = active() ? active() : root_;
    Span span(
        trace_,
        "custom.python",
        Level::operations,
        node ? node->id : 0,
        node ? node->source_id : "",
        operation.empty() && node ? node->name : operation
    );
    try {
        RecursionGuard guard(" while calling a Syntwine Python operation");
        return fn(*args, **kwargs);
    } catch (Error& error) {
        span.fail(kind_name_for_error(error.code));
        annotate(error);
        throw;
    } catch (const py::error_already_set& cause) {
        Error error(ErrorCode::operation, "Python operation failed: " + std::string(cause.what()));
        error.cause = cause.value();
        span.fail("OperationError");
        annotate(error);
        throw error;
    }
}

Usage
Session::validate_budget(std::string_view bytes, const Budget& budget, bool count_usage) {
    return validate(bytes, budget, count_usage, {});
}

Usage
Session::validate_budget(const Bytes& bytes, const Budget& budget, bool count_usage) {
    return validate(bytes.data(), budget, count_usage, bytes.data_);
}

Usage
Session::validate(
    std::string_view bytes,
    const Budget& budget,
    bool count_usage,
    const std::shared_ptr<const std::string>& stable_bytes
) {
    Usage usage{};
    usage.bytes = bytes.size();
    const auto* node = active() ? active() : root_;
    auto check = [&](std::string_view dimension,
                     std::size_t actual,
                     std::optional<std::size_t> limit,
                     Span& span) {
        if (!limit || actual <= *limit)
            return;
        Error error(
            ErrorCode::budget,
            std::string(dimension)
                + " budget exceeded: "
                + std::to_string(actual)
                + " > "
                + std::to_string(*limit)
        );
        error.dimension = dimension;
        error.actual = actual;
        error.budget = limit;
        span.fail("BudgetExceeded");
        throw error;
    };
    auto make_span = [&](std::string_view name) {
        return Span(
            trace_,
            name,
            Level::stages,
            node ? node->id : 0,
            node ? node->source_id : "",
            node ? node->name : ""
        );
    };
    if (count_usage) {
        auto span = make_span("output.validate");
        usage.characters = utf8_characters(bytes);
        span.input_bytes(usage.bytes);
        span.characters(usage.characters);
    }
    {
        auto span = make_span("budget.bytes");
        span.output_bytes(usage.bytes);
        check("bytes", usage.bytes, budget.bytes, span);
    }
    {
        auto span = make_span("budget.characters");
        if (!count_usage && (budget.characters || budget.tokens))
            usage.characters = utf8_characters(bytes);
        if (!usage.characters && (budget.characters || budget.tokens)) {
            span.fail("DecodeError");
            throw Error(ErrorCode::decode, "character and token budgets require valid UTF-8");
        }
        if (usage.characters) {
            span.characters(*usage.characters);
            check("characters", *usage.characters, budget.characters, span);
        }
    }
    if (!budget.tokens && (!count_usage || tokenizer_.is_none() || !usage.characters))
        return usage;
    auto span = make_span("budget.tokens");
    if (tokenizer_.is_none()) {
        span.fail("TypeMismatch");
        throw Error(ErrorCode::type, "a token budget requires an explicit tokenizer");
    }
    if (stable_bytes && stable_bytes == last_token_bytes_) {
        usage.tokens = last_token_count_;
        span.cache_hit(true);
    } else if (PyUnicode_Check(tokenizer_.ptr())) {
        const auto encoding = py::cast<std::string>(tokenizer_);
        try {
            py::gil_scoped_release release;
            usage.tokens = token_count(bytes, encoding);
        } catch (const std::exception& cause) {
            span.fail("OperationError");
            throw Error(ErrorCode::operation, cause.what());
        }
    } else {
        auto fn = PyCallable_Check(tokenizer_.ptr()) ? tokenizer_ : tokenizer_.attr("encode");
        auto count = call(fn, py::make_tuple(py::str(bytes)), py::dict(), "tokenizer");
        if (PyLong_Check(count.ptr()) && !PyBool_Check(count.ptr())) {
            const auto n = PyLong_AsSize_t(count.ptr());
            if (PyErr_Occurred()) {
                PyErr_Clear();
                throw Error(ErrorCode::type, "tokenizer returned a count outside the size_t range");
            }
            usage.tokens = n;
        } else {
            usage.tokens = py::len(count);
        }
    }
    if (stable_bytes) {
        last_token_bytes_ = stable_bytes;
        last_token_count_ = *usage.tokens;
    }
    span.tokens(*usage.tokens);
    check("tokens", *usage.tokens, budget.tokens, span);
    return usage;
}

} // namespace syntwine
