#include "syntwine/detail/telemetry/telemetry_internal.hpp"
#include "syntwine/detail/platform/utf8.hpp"

#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <mutex>
#include <random>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace syntwine::telemetry {
namespace py = pybind11;

namespace {

using Steady = std::chrono::steady_clock;
using Wall = std::chrono::system_clock;
using Nanoseconds = std::chrono::nanoseconds;

template<typename Clock>
std::uint64_t now() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct Config {
    bool enabled, traces, metrics, events;
    Level level;
    double sample_rate;
    bool include_content;
    std::shared_ptr<Sink> sink;
    std::size_t max_events, max_content_bytes;
};

constexpr std::size_t default_max_content_bytes = 65536;
constexpr std::string_view levels[] = {"build", "stages", "operations"};

Level parse_level(std::string_view name) {
    for (unsigned i = 0; i < std::size(levels); ++i) {
        if (name == levels[i])
            return static_cast<Level>(i);
    }
    throw py::value_error("telemetry level must be build, stages, or operations");
}

Config make_config(bool enabled, bool traces, bool metrics, bool events,
                   std::string_view level, double sample_rate, bool include_content,
                   std::shared_ptr<Sink> sink, std::size_t max_events,
                   std::size_t max_content_bytes) {
    if (!std::isfinite(sample_rate) || sample_rate < 0 || sample_rate > 1)
        throw py::value_error("sample_rate must be between 0 and 1");
    return Config{enabled, traces, metrics, events, parse_level(level), sample_rate,
                  include_content, std::move(sink), max_events, max_content_bytes};
}

struct Controls {
    py::object context;
    std::atomic<bool> has_context = false;
    mutable std::mutex global_mutex;
    std::optional<Config> global;
};

const std::string controls_key = "syntwine.telemetry.controls";
std::atomic<bool> has_default_or_context = false;

// pybind11 uses interpreter-local internals; no Python handles survive interpreter teardown.
Controls* controls() {
    return static_cast<Controls*>(py::get_shared_data(controls_key));
}

std::optional<Config> get_config(py::handle explicit_config) {
    if (explicit_config && !explicit_config.is_none())
        return explicit_config.cast<Config>();
    if (!has_default_or_context.load(std::memory_order_acquire))
        return std::nullopt;
    const auto* ctl = controls();
    if (!ctl)
        return std::nullopt;
    if (ctl->has_context.load(std::memory_order_acquire)) {
        PyObject* object = nullptr;
        if (PyContextVar_Get(ctl->context.ptr(), Py_None, &object) < 0)
            throw py::error_already_set();
        auto config = py::reinterpret_steal<py::object>(object);
        if (!config.is_none())
            return config.cast<Config>();
    }
    const std::lock_guard guard(ctl->global_mutex);
    return ctl->global;
}

std::mt19937_64 seed_generator() {
    std::random_device random;
    std::seed_seq seed{random(), random(), random(), random(), random(), random()};
    return std::mt19937_64(seed);
}

std::mt19937_64& random_generator() {
    static thread_local std::mt19937_64 engine(seed_generator());
#ifndef _WIN32
    // A fork copies the PRNG state, but parent and child export to the same collector.
    const auto pid = getpid();
    static thread_local auto seeded_for = pid;
    if (seeded_for != pid) {
        engine = seed_generator();
        seeded_for = pid;
    }
#endif
    return engine;
}

std::string make_trace_id(std::mt19937_64& generator) {
    const auto high = generator();
    const auto low = generator();
    return hex_id(high) + hex_id(high || low ? low : 1);
}

std::string make_build_id(std::mt19937_64& generator) {
    static std::atomic<std::uint64_t> counter = 0;
    return hex_id(generator()) + hex_id(counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

std::string base64(std::string_view bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[i++]);
        const bool second_present = i < bytes.size();
        const unsigned second = second_present ? static_cast<unsigned char>(bytes[i++]) : 0;
        const bool third_present = i < bytes.size();
        const unsigned third = third_present ? static_cast<unsigned char>(bytes[i++]) : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 3) << 4) | (second >> 4)]);
        output.push_back(second_present ? alphabet[((second & 15) << 2) | (third >> 6)] : '=');
        output.push_back(third_present ? alphabet[third & 63] : '=');
    }
    return output;
}

std::string metadata(std::string_view value) {
    if (utf8_characters(value))
        return std::string(value);
    std::string result;
    for (unsigned char c : value) {
        if (c < 0x80) {
            result += static_cast<char>(c);
            continue;
        }
        constexpr std::string_view hex = "0123456789abcdef";
        result += "\\x";
        result += hex[c >> 4];
        result += hex[c & 15];
    }
    return result;
}
} // namespace

struct Trace::State {
    State(Config settings, std::mt19937_64& generator, std::uint64_t node_id, std::string_view source_id,
          std::string_view operation) : config(std::move(settings)),
          build(std::make_shared<Build>()), steady_start(now<Steady>()),
          limit(config.sink ? std::min(config.max_events, config.sink->capacity() - 1)
                            : config.max_events) {
        build->start = now<Wall>();
        build->trace_id = make_trace_id(generator);
        build->build_id = make_build_id(generator);
        if (config.metrics)
            build->metrics.emplace();
        if (config.traces) {
            Record root;
            root.name = "generate";
            root.id = 1;
            root.start = build->start;
            root.node = node_id;
            root.source_id = metadata(source_id);
            root.operation = metadata(operation);
            build->spans.push_back(std::move(root));
            active = 0;
        }
    }

    const Record* active_span() const noexcept {
        return active < build->spans.size() ? &build->spans[active] : nullptr;
    }

    bool detail_available() noexcept {
        if (details < limit)
            return true;
        ++build->dropped_events;
        return false;
    }

    Record make_record(std::string_view name, std::uint64_t timestamp, std::uint64_t node_id,
                       std::string_view source_id, std::string_view operation) const {
        const auto* parent = active_span();
        Record record;
        record.name = metadata(name);
        record.id = next_id;
        record.parent = parent ? parent->id : 0;
        record.node = node_id ? node_id : (parent ? parent->node : 0);
        record.source_id = source_id.empty() && parent ? parent->source_id : metadata(source_id);
        record.operation = operation.empty() ? record.name : metadata(operation);
        record.start = build->start + (timestamp - steady_start);
        return record;
    }

    Record* add_event(std::string_view name, std::uint64_t node_id, std::string_view source_id,
                      std::string_view operation) noexcept {
        if (!config.events || !detail_available())
            return nullptr;
        try {
            build->events.push_back(make_record(name, now<Steady>(), node_id, source_id, operation));
            ++details;
            ++next_id;
            return &build->events.back();
        } catch (...) {
            ++build->dropped_events;
            return nullptr;
        }
    }

    Config config;
    std::shared_ptr<Build> build;
    std::uint64_t steady_start;
    std::size_t limit, details = 0, active = static_cast<std::size_t>(-1);
    std::uint64_t next_id = 2;
    bool finished = false;
};

Trace::Trace(std::unique_ptr<State> state) : state_(std::move(state)) {}
Trace::~Trace() { finish("OperationError"); }

const std::string& Trace::id() const noexcept { return state_->build->trace_id; }

bool Trace::records_dependencies() const noexcept {
    const auto& state = *state_;
    return !state.finished && state.config.level != Level::build &&
           (state.config.traces || state.config.events);
}

void Trace::event(std::string_view name, std::uint64_t node_id, std::string_view source_id,
                  std::string_view operation) noexcept {
    if (!state_->finished)
        state_->add_event(name, node_id, source_id, operation);
}

void Trace::dependency(std::uint64_t parent, std::uint64_t child, bool discovered) noexcept {
    auto& state = *state_;
    if (!records_dependencies())
        return;
    if (state.detail_available()) {
        try {
            state.build->dependencies.push_back({parent, child, discovered});
            ++state.details;
        } catch (...) {
            ++state.build->dropped_events;
        }
    }
    if (discovered) {
        if (auto* event = state.add_event("dependency.discover", parent, {}, {})) {
            event->dependency_node = child;
            event->discovered_dependencies = 1;
        }
    }
}

void Trace::finish(std::string_view error_code, std::optional<std::string_view> content) noexcept {
    auto& state = *state_;
    if (state.finished)
        return;
    state.finished = true;
    auto& build = *state.build;
    build.duration = now<Steady>() - state.steady_start;
    if (content)
        build.output_bytes = content->size();
    try {
        build.error_code = metadata(error_code);
        if (!build.spans.empty()) {
            auto& root = build.spans.front();
            root.duration = build.duration;
            root.output = build.output_bytes;
            root.error_code = build.error_code;
        }
        if (auto* record = state.add_event("build.finish", 0, {}, {})) {
            record->start = build.start + build.duration;
            record->output = build.output_bytes;
            record->error_code = build.error_code;
        }
        if (state.config.include_content && content) {
            if (content->size() <= state.config.max_content_bytes)
                build.content_base64 = base64(*content);
            else
                build.content_omitted_bytes = content->size();
        }
        if (state.config.sink)
            state.config.sink->submit(state.build);
    } catch (...) {
        // Instrumentation must not change the result of a build.
    }
}

py::dict Trace::view() const {
    return build_python(*state_->build);
}

void Span::begin(std::string_view name, Level level, std::uint64_t node_id,
                 std::string_view source_id, std::string_view operation) noexcept {
    auto& state = *trace_->state_;
    if (state.finished) {
        trace_ = nullptr;
        return;
    }
    output_stage_ = name == "output.serialize";
    if (state.build->metrics) {
        if (name == "node.evaluate")
            metric_ = Metric::node;
        else if (name == "custom.python")
            metric_ = Metric::python;
        else if (name == "budget.tokens")
            metric_ = Metric::token;
        else if (name == "transform" || name == "squish" || name.starts_with("parse.") ||
                 name == "custom.native")
            metric_ = Metric::native;
    }
    if (state.config.traces && level <= state.config.level && state.detail_available()) {
        try {
            start_ = now<Steady>();
            auto& records = state.build->spans;
            records.push_back(state.make_record(name, start_, node_id, source_id, operation));
            record_ = records.size() - 1;
            previous_ = state.active;
            state.active = record_;
            ++state.details;
            ++state.next_id;
            exceptions_ = std::uncaught_exceptions();
        } catch (...) {
            ++state.build->dropped_events;
        }
    }
    if (!start_ && (metric_ == Metric::native || metric_ == Metric::python || metric_ == Metric::token))
        start_ = now<Steady>();
}

void Span::end() noexcept {
    const auto duration = start_ ? now<Steady>() - start_ : 0;
    auto& state = *trace_->state_;
    if (record_ != no_record) {
        auto& record = state.build->spans[record_];
        record.duration = duration;
        if (std::uncaught_exceptions() > exceptions_ && record.error_code.empty())
            fail("OperationError");
        state.active = previous_;
    }
    if (!state.build->metrics)
        return;
    auto& metrics = *state.build->metrics;
    switch (metric_) {
    case Metric::node:
        metrics.nodes += !cached_;
        break;
    case Metric::native:
        metrics.native_ns += duration;
        break;
    case Metric::python:
        metrics.python_ns += duration;
        break;
    case Metric::token:
        metrics.token_ns += duration;
        break;
    case Metric::none:
        break;
    }
}

void Span::fail(std::string_view code) noexcept {
    if (record_ == no_record)
        return;
    try {
        trace_->state_->build->spans[record_].error_code = metadata(code);
    } catch (...) {
    }
}

void Span::input_bytes(std::optional<std::size_t> value) noexcept {
    if (record_ != no_record)
        trace_->state_->build->spans[record_].input = value;
}

void Span::output_bytes(std::optional<std::size_t> value) noexcept {
    if (output_stage_)
        trace_->state_->build->output_bytes = value;
    if (record_ != no_record)
        trace_->state_->build->spans[record_].output = value;
}

void Span::characters(std::optional<std::size_t> value) noexcept {
    if (record_ != no_record)
        trace_->state_->build->spans[record_].characters = value;
}

void Span::tokens(std::optional<std::size_t> value) noexcept {
    if (record_ != no_record)
        trace_->state_->build->spans[record_].tokens = value;
}

void Span::cache_hit(bool value) noexcept {
    if (value && !cached_ && trace_ && trace_->state_->build->metrics)
        ++trace_->state_->build->metrics->cache_hits;
    cached_ = value;
    if (record_ != no_record)
        trace_->state_->build->spans[record_].cache_hit = value;
}

void Span::discovered_dependencies(std::size_t value) noexcept {
    if (record_ != no_record)
        trace_->state_->build->spans[record_].discovered_dependencies = value;
}

std::unique_ptr<Trace> start(py::handle explicit_config, std::uint64_t node_id,
                             std::string_view source_id, std::string_view operation) {
    auto config = get_config(explicit_config);
    if (!config || !config->enabled || config->sample_rate == 0)
        return {};
    try {
        auto& generator = random_generator();
        if (config->sample_rate < 1 &&
            std::generate_canonical<double, 53>(generator) >= config->sample_rate)
            return {};
        return std::make_unique<Trace>(std::make_unique<Trace::State>(
            std::move(*config), generator, node_id, source_id, operation));
    } catch (...) {
        return {};
    }
}

void bind(py::module_& module) {
    auto mod = module.def_submodule("_telemetry");
    auto context = py::reinterpret_steal<py::object>(PyContextVar_New("syntwine.telemetry", Py_None));
    if (!context)
        throw py::error_already_set();
    auto state = std::make_unique<Controls>();
    state->context = std::move(context);
    auto capsule = py::capsule(state.get(), [](void* pointer) {
        delete static_cast<Controls*>(pointer);
    });
    auto* ctl = state.release();
    mod.attr("_state") = std::move(capsule);
    py::set_shared_data(controls_key, ctl);

    py::class_<Sink, std::shared_ptr<Sink>>(mod, "Sink")
        .def_property_readonly("capacity", &Sink::capacity)
        .def_property_readonly("dropped_events", &Sink::dropped_events)
        .def("flush", &Sink::flush, py::call_guard<py::gil_scoped_release>());
    const auto snapshot = [](const MemorySink& sink) {
        py::list result;
        for (const auto& build : sink.snapshot())
            result.append(build_python(*build));
        return result;
    };
    py::class_<MemorySink, Sink, std::shared_ptr<MemorySink>>(mod, "MemorySink")
        .def(py::init<std::size_t>(), py::arg("capacity") = default_capacity)
        .def_property_readonly("records", snapshot)
        .def("snapshot", snapshot)
        .def("clear", &MemorySink::clear);
    py::class_<JSONLSink, Sink, std::shared_ptr<JSONLSink>>(mod, "JSONLSink")
        .def(py::init<std::filesystem::path, std::size_t>(), py::arg("path"),
             py::arg("capacity") = default_capacity)
        .def_property_readonly("path", &JSONLSink::path);
    py::class_<SQLiteSink, Sink, std::shared_ptr<SQLiteSink>>(mod, "SQLiteSink")
        .def(py::init<std::filesystem::path, std::size_t>(), py::arg("path"),
             py::arg("capacity") = default_capacity)
        .def_property_readonly("path", &SQLiteSink::path);
    py::class_<OTLPSink, Sink, std::shared_ptr<OTLPSink>>(mod, "OTLPSink")
        .def(py::init([](std::string endpoint, std::optional<OTLPSink::Headers> headers,
                         double timeout, std::size_t capacity) {
            return std::make_shared<OTLPSink>(std::move(endpoint),
                headers ? std::move(*headers) : OTLPSink::Headers{}, timeout, capacity);
        }), py::arg("endpoint"), py::arg("headers") = py::none(), py::arg("timeout") = 5.0,
            py::arg("capacity") = default_capacity)
        .def_property_readonly("endpoint", &OTLPSink::endpoint)
        .def_property_readonly("timeout", &OTLPSink::timeout)
        .def_property_readonly("rejected_records", &OTLPSink::rejected_records)
        .def_property_readonly("last_export_warning", &OTLPSink::last_export_warning);
    py::class_<Config>(mod, "TelemetryConfig")
        .def(py::init(&make_config), py::kw_only(), py::arg("enabled") = false,
             py::arg("traces") = true, py::arg("metrics") = true, py::arg("events") = true,
             py::arg("level") = "operations", py::arg("sample_rate") = 1.0,
             py::arg("include_content") = false, py::arg("sink") = nullptr,
             py::arg("max_events") = default_max_events,
             py::arg("max_content_bytes") = default_max_content_bytes)
        .def_readonly("enabled", &Config::enabled)
        .def_readonly("traces", &Config::traces)
        .def_readonly("metrics", &Config::metrics)
        .def_readonly("events", &Config::events)
        .def_property_readonly("level", [](const Config& config) {
            return levels[static_cast<unsigned>(config.level)];
        })
        .def_readonly("sample_rate", &Config::sample_rate)
        .def_readonly("include_content", &Config::include_content)
        .def_readonly("sink", &Config::sink)
        .def_readonly("max_events", &Config::max_events)
        .def_readonly("max_content_bytes", &Config::max_content_bytes);
    mod.def("configure", [](const Config& config) {
        auto* ctl = controls();
        const std::lock_guard guard(ctl->global_mutex);
        ctl->global = config.enabled ? std::optional(config) : std::nullopt;
        if (config.enabled)
            has_default_or_context.store(true, std::memory_order_release);
    }, py::arg("config"));
    mod.def("_push_config", [](const Config& config) {
        auto* ctl = controls();
        auto value = py::cast(config);
        auto token = py::reinterpret_steal<py::object>(PyContextVar_Set(ctl->context.ptr(), value.ptr()));
        if (!token)
            throw py::error_already_set();
        ctl->has_context.store(true, std::memory_order_release);
        has_default_or_context.store(true, std::memory_order_release);
        return token;
    });
    mod.def("_reset_config", [](py::handle token) {
        if (PyContextVar_Reset(controls()->context.ptr(), token.ptr()) < 0)
            throw py::error_already_set();
    });
    mod.def("flush", &flush_sinks, py::call_guard<py::gil_scoped_release>());
}
} // namespace syntwine::telemetry
