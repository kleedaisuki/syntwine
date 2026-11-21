#include "syntwine/detail/telemetry/telemetry_internal.hpp"

#include <curl/curl.h>
#include <jsoncons/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace syntwine::telemetry {
namespace py = pybind11;
using Json = jsoncons::ojson;

namespace {
Json
object() {
    return Json(jsoncons::json_object_arg);
}

Json
array() {
    return Json(jsoncons::json_array_arg);
}

template <class T>
Json
optional_json(const std::optional<T>& item) {
    return item ? Json(*item) : Json::null();
}

Json
optional_text(const std::string& item) {
    return item.empty() ? Json::null() : Json(item);
}

std::string_view
status(const std::string& code) {
    return code.empty() ? "ok" : "error";
}

std::string
dump(const Json& json) {
    std::string result;
    json.dump(result);
    return result;
}

void
make_parent(const std::filesystem::path& path) {
    if (const auto parent = path.parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);
}

std::string
utf8_path(const std::filesystem::path& path) {
    const auto name = path.u8string();
    return std::string(name.begin(), name.end());
}

struct PendingSinks {
    std::mutex mutex;
    std::unordered_map<Sink*, std::shared_ptr<Sink>> sinks;
};

PendingSinks&
pending_sinks() {
    static PendingSinks pending;
    return pending;
}
} // namespace

std::string
hex_id(std::uint64_t value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output(16, '0');
    for (std::size_t i = output.size(); i > 0; --i, value >>= 4)
        output[i - 1] = digits[value & 15];
    return output;
}

namespace {
Json
record_json(const Build& build, const Record& record, bool is_span) {
    auto out = object();
    out["trace_id"] = build.trace_id;
    out["build_id"] = build.build_id;
    out["span_id"] = hex_id(record.id);
    out["parent_span_id"] = record.parent ? Json(hex_id(record.parent)) : Json::null();
    out["node_id"] = record.node ? Json(record.node) : Json::null();
    out["source_id"] = optional_text(record.source_id);
    out["operation"] = record.operation.empty() ? record.name : record.operation;
    out["type"] = is_span ? "span" : "event";
    out["name"] = record.name;
    out["status"] = status(record.error_code);
    out["error_code"] = optional_text(record.error_code);
    out["start_ns"] = record.start;
    out["duration_ns"] = record.duration;
    out["input_bytes"] = optional_json(record.input);
    out["output_bytes"] = optional_json(record.output);
    out["characters"] = optional_json(record.characters);
    out["tokens"] = optional_json(record.tokens);
    out["cache_hit"] = optional_json(record.cache_hit);
    out["discovered_dependencies"] = optional_json(record.discovered_dependencies);
    if (record.dependency_node)
        out["dependency_node_id"] = record.dependency_node;
    return out;
}

Json
metrics_json(const Build& build) {
    auto out = object();
    if (!build.metrics)
        return out;
    out["builds"] = 1;
    out["failed_builds"] = static_cast<unsigned>(!build.error_code.empty());
    out["build_duration_ns"] = build.duration;
    if (build.output_bytes)
        out["output_bytes"] = *build.output_bytes;
    out["nodes"] = build.metrics->nodes;
    out["cache_hits"] = build.metrics->cache_hits;
    out["native_operations_duration_ns"] = build.metrics->native_ns;
    out["python_callbacks_duration_ns"] = build.metrics->python_ns;
    out["token_duration_ns"] = build.metrics->token_ns;
    out["dropped_events"] = build.dropped_events;
    out["sink_dropped_events"] = build.sink_dropped_events;
    return out;
}

Json
build_json(const Build& build) {
    auto out = object();
    out["trace_id"] = build.trace_id;
    out["build_id"] = build.build_id;
    out["type"] = "build";
    out["status"] = status(build.error_code);
    out["error_code"] = optional_text(build.error_code);
    out["start_ns"] = build.start;
    out["duration_ns"] = build.duration;
    out["output_bytes"] = optional_json(build.output_bytes);
    out["dropped_events"] = build.dropped_events;
    out["sink_dropped_events"] = build.sink_dropped_events;
    if (build.content_base64)
        out["content_base64"] = *build.content_base64;
    if (build.content_omitted_bytes)
        out["content_omitted_bytes"] = build.content_omitted_bytes;
    auto spans = array();
    for (const auto& span : build.spans)
        spans.push_back(record_json(build, span, true));
    out["spans"] = std::move(spans);
    auto events = array();
    for (const auto& event : build.events)
        events.push_back(record_json(build, event, false));
    out["events"] = std::move(events);
    auto dependencies = array();
    for (const auto& edge : build.dependencies) {
        auto value = object();
        value["parent_node_id"] = edge.parent;
        value["child_node_id"] = edge.child;
        value["discovered"] = edge.discovered;
        dependencies.push_back(std::move(value));
    }
    out["dependencies"] = std::move(dependencies);
    out["metrics"] = metrics_json(build);
    return out;
}

py::object
to_python(const Json& item) {
    if (item.is_null())
        return py::none();
    if (item.is_bool())
        return py::bool_(item.as<bool>());
    if (item.is_number())
        return py::int_(item.as<std::uint64_t>());
    if (item.is_string()) {
        const auto value = item.as_string_view();
        return py::str(value.data(), value.size());
    }
    if (item.is_array()) {
        py::list result(item.size());
        std::size_t index = 0;
        for (const auto& value : item.array_range())
            result[index++] = to_python(value);
        return result;
    }
    py::dict result;
    for (const auto& member : item.object_range())
        result[py::str(member.key())] = to_python(member.value());
    return result;
}
} // namespace

py::dict
build_python(const Build& build) {
    return to_python(build_json(build)).cast<py::dict>();
}

Sink::Sink(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity == 0)
        throw std::invalid_argument("sink capacity must be positive");
}

void
Sink::track() {
    if (tracked_ || retains_records())
        return;
    auto& pending = pending_sinks();
    const std::lock_guard guard(pending.mutex);
    pending.sinks.emplace(this, shared_from_this());
    tracked_ = true;
}

void
Sink::untrack() {
    if (!tracked_)
        return;
    auto& pending = pending_sinks();
    const std::lock_guard guard(pending.mutex);
    pending.sinks.erase(this);
    tracked_ = false;
}

void
Sink::evict() {
    const auto weight = queue_.front()->weight();
    weight_ -= weight;
    dropped_ += weight;
    queue_.pop_front();
}

void
Sink::submit(std::shared_ptr<Build> build) {
    const std::lock_guard guard(mutex_);
    track();
    const auto weight = build->weight();
    queue_.push_back({build});
    weight_ += weight;
    dropped_ += build->dropped_events;
    while (weight_ > capacity_)
        evict();
    build->sink_dropped_events = dropped_;
}

Batch
Sink::snapshot() const {
    const std::lock_guard guard(mutex_);
    return queue_;
}

void
Sink::clear() {
    const std::lock_guard flush_guard(flush_mutex_);
    const std::lock_guard guard(mutex_);
    queue_.clear();
    weight_ = dropped_ = 0;
    untrack();
}

std::size_t
Sink::dropped_events() const {
    const std::lock_guard guard(mutex_);
    return dropped_;
}

void
Sink::flush() {
    if (retains_records())
        return;
    const std::lock_guard flush_guard(flush_mutex_);
    Batch batch;
    {
        const std::lock_guard guard(mutex_);
        if (queue_.empty()) {
            untrack();
            return;
        }
        batch.swap(queue_);
        weight_ = 0;
    }
    try {
        export_batch(batch);
    } catch (...) {
        const std::lock_guard guard(mutex_);
        while (!batch.empty()) {
            if (batch.back().pending_signals == 0) {
                batch.pop_back();
                continue;
            }
            const auto weight = batch.back()->weight();
            if (weight <= capacity_ - weight_) {
                queue_.push_front(std::move(batch.back()));
                weight_ += weight;
            } else {
                dropped_ += weight;
            }
            batch.pop_back();
        }
        if (queue_.empty())
            untrack();
        throw;
    }
    const std::lock_guard guard(mutex_);
    if (queue_.empty())
        untrack();
}

void
flush_sinks() {
    std::vector<std::shared_ptr<Sink>> sinks;
    {
        auto& pending = pending_sinks();
        const std::lock_guard guard(pending.mutex);
        sinks.reserve(pending.sinks.size());
        for (const auto& [_, sink] : pending.sinks)
            sinks.push_back(sink);
    }
    std::exception_ptr failure;
    for (const auto& sink : sinks) {
        try {
            sink->flush();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    }
    if (failure)
        std::rethrow_exception(failure);
}

void
JSONLSink::export_batch(Batch& batch) {
    static std::mutex append_mutex;
    const std::lock_guard guard(append_mutex);
    make_parent(path_);
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    for (const auto& build : batch) {
        build_json(*build).dump(out);
        out.put('\n');
    }
    out.close();
}

namespace {

bool
pending(const QueuedBuild& build, Signal signal) noexcept {
    return (build.pending_signals & static_cast<unsigned>(signal)) != 0;
}

void
acknowledge(Batch& batch, Signal signal) noexcept {
    for (auto& build : batch)
        build.pending_signals &= ~static_cast<unsigned>(signal);
}

void
sqlite_check(int result, sqlite3* connection) {
    if (result != SQLITE_OK)
        throw std::runtime_error(
            std::string("SQLite telemetry: ")
            + (connection ? sqlite3_errmsg(connection) : sqlite3_errstr(result))
        );
}

void
sqlite_execute(sqlite3* connection, const char* sql) {
    sqlite_check(sqlite3_exec(connection, sql, nullptr, nullptr, nullptr), connection);
}

class Statement {
public:
    Statement(sqlite3* connection, const char* sql)
        : db_(connection),
          statement_(nullptr, sqlite3_finalize) {
        sqlite3_stmt* raw = nullptr;
        const auto status = sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
        statement_.reset(raw);
        sqlite_check(status, db_);
    }

    template <class... Values>
    void
    write(const Values&... values) {
        int column = 1;
        (bind(column++, values), ...);
        const auto result = sqlite3_step(statement_.get());
        if (result != SQLITE_DONE)
            sqlite_check(result, db_);
        sqlite_check(sqlite3_reset(statement_.get()), db_);
        sqlite_check(sqlite3_clear_bindings(statement_.get()), db_);
    }

private:
    void
    bind(int column, std::string_view value) {
        sqlite_check(
            sqlite3_bind_text64(
                statement_.get(),
                column,
                value.data(),
                value.size(),
                SQLITE_TRANSIENT,
                SQLITE_UTF8
            ),
            db_
        );
    }

    void
    bind(int column, std::uint64_t value) {
        sqlite_check(
            sqlite3_bind_int64(statement_.get(), column, static_cast<sqlite3_int64>(value)),
            db_
        );
    }

    template <class T>
    void
    bind(int column, const std::optional<T>& value) {
        if (value)
            bind(column, *value);
        else
            sqlite_check(sqlite3_bind_null(statement_.get(), column), db_);
    }

    sqlite3* db_;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement_;
};

constexpr auto schema = R"SQL(
    CREATE TABLE IF NOT EXISTS builds(
        trace_id TEXT PRIMARY KEY, build_id TEXT NOT NULL UNIQUE, status TEXT NOT NULL,
        start_ns INTEGER NOT NULL, duration_ns INTEGER NOT NULL, output_bytes INTEGER,
        error_code TEXT, dropped_events INTEGER NOT NULL, payload TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS spans(
        trace_id TEXT NOT NULL REFERENCES builds(trace_id), span_id TEXT NOT NULL,
        parent_span_id TEXT, build_id TEXT NOT NULL, node_id INTEGER, source_id TEXT,
        operation TEXT NOT NULL, name TEXT NOT NULL, status TEXT NOT NULL,
        start_ns INTEGER NOT NULL, duration_ns INTEGER NOT NULL, error_code TEXT,
        input_bytes INTEGER, output_bytes INTEGER, payload TEXT NOT NULL,
        PRIMARY KEY(trace_id,span_id)
    );
    CREATE TABLE IF NOT EXISTS events(
        trace_id TEXT NOT NULL REFERENCES builds(trace_id), span_id TEXT NOT NULL,
        parent_span_id TEXT, build_id TEXT NOT NULL, node_id INTEGER, source_id TEXT,
        operation TEXT NOT NULL, name TEXT NOT NULL, status TEXT NOT NULL,
        start_ns INTEGER NOT NULL, duration_ns INTEGER NOT NULL, error_code TEXT,
        input_bytes INTEGER, output_bytes INTEGER, payload TEXT NOT NULL,
        PRIMARY KEY(trace_id,span_id)
    );
    CREATE TABLE IF NOT EXISTS metrics(
        trace_id TEXT NOT NULL REFERENCES builds(trace_id), build_id TEXT NOT NULL,
        name TEXT NOT NULL, value INTEGER NOT NULL, PRIMARY KEY(trace_id,name)
    );
    CREATE INDEX IF NOT EXISTS spans_name ON spans(name);
    CREATE INDEX IF NOT EXISTS metrics_name ON metrics(name);
)SQL";

std::optional<std::string_view>
nonempty(std::string_view item) {
    return item.empty() ? std::nullopt : std::optional(item);
}

void
write_records(
    Statement& write,
    const Build& build,
    const std::vector<Record>& records,
    bool is_span
) {
    for (const auto& record : records) {
        const auto parent = record.parent ? std::optional(hex_id(record.parent)) : std::nullopt;
        const auto node = record.node ? std::optional(record.node) : std::nullopt;
        write.write(
            build.trace_id,
            hex_id(record.id),
            parent,
            build.build_id,
            node,
            nonempty(record.source_id),
            record.operation.empty() ? record.name : record.operation,
            record.name,
            status(record.error_code),
            record.start,
            record.duration,
            nonempty(record.error_code),
            record.input,
            record.output,
            dump(record_json(build, record, is_span))
        );
    }
}
} // namespace

void
SQLiteSink::export_batch(Batch& batch) {
    make_parent(path_);
    sqlite3* raw = nullptr;
    const auto filename = utf8_path(path_);
    const auto result = sqlite3_open_v2(
        filename.c_str(),
        &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE,
        nullptr
    );
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    sqlite_check(result, raw);
    sqlite_check(sqlite3_busy_timeout(raw, 5000), raw);
    sqlite_execute(raw, "PRAGMA foreign_keys = ON; BEGIN IMMEDIATE");
    sqlite_execute(raw, schema);
    Statement builds(raw, "INSERT OR REPLACE INTO builds VALUES(?,?,?,?,?,?,?,?,?)");
    Statement spans(raw, "INSERT OR REPLACE INTO spans VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Statement events(raw, "INSERT OR REPLACE INTO events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Statement metrics(raw, "INSERT OR REPLACE INTO metrics VALUES(?,?,?,?)");

    for (const auto& build : batch) {
        builds.write(
            build->trace_id,
            build->build_id,
            status(build->error_code),
            build->start,
            build->duration,
            build->output_bytes,
            nonempty(build->error_code),
            build->dropped_events,
            dump(build_json(*build))
        );
        write_records(spans, *build, build->spans, true);
        write_records(events, *build, build->events, false);
        const auto values = metrics_json(*build);
        for (const auto& item : values.object_range())
            metrics.write(
                build->trace_id,
                build->build_id,
                item.key(),
                item.value().as<std::uint64_t>()
            );
    }
    sqlite_execute(raw, "COMMIT");
}

namespace {

void
attribute(Json& attributes, std::string_view name, const Json& value) {
    if (value.is_null())
        return;
    auto item = object();
    item["key"] = name;
    auto typed = object();
    if (value.is_bool())
        typed["boolValue"] = value;
    else if (value.is_number())
        typed["intValue"] = std::to_string(value.as<std::uint64_t>());
    else
        typed["stringValue"] = value;
    item["value"] = std::move(typed);
    attributes.push_back(std::move(item));
}

Json
otlp_attributes(const Build& build, const Record& record) {
    auto attrs = array();
    attribute(attrs, "syntwine.build_id", build.build_id);
    attribute(
        attrs,
        "syntwine.operation",
        record.operation.empty() ? record.name : record.operation
    );
    attribute(attrs, "syntwine.error_code", optional_text(record.error_code));
    attribute(attrs, "syntwine.source_id", optional_text(record.source_id));
    if (record.node)
        attribute(attrs, "syntwine.node_id", record.node);
    if (record.dependency_node)
        attribute(attrs, "syntwine.dependency_node_id", record.dependency_node);
    attribute(attrs, "syntwine.input_bytes", optional_json(record.input));
    attribute(attrs, "syntwine.output_bytes", optional_json(record.output));
    attribute(attrs, "syntwine.characters", optional_json(record.characters));
    attribute(attrs, "syntwine.tokens", optional_json(record.tokens));
    attribute(attrs, "syntwine.cache_hit", optional_json(record.cache_hit));
    attribute(
        attrs,
        "syntwine.discovered_dependencies",
        optional_json(record.discovered_dependencies)
    );
    return attrs;
}

Json
otlp_span(const Build& build, const Record& record) {
    auto span = object();
    span["traceId"] = build.trace_id;
    span["spanId"] = hex_id(record.id);
    if (record.parent)
        span["parentSpanId"] = hex_id(record.parent);
    span["name"] = record.name;
    span["kind"] = 1;
    span["startTimeUnixNano"] = std::to_string(record.start);
    span["endTimeUnixNano"] = std::to_string(record.start + record.duration);
    span["flags"] = 1;
    auto state = object();
    state["code"] = record.error_code.empty() ? 1 : 2;
    span["status"] = std::move(state);
    auto attrs = otlp_attributes(build, record);
    if (!record.parent) {
        if (build.content_base64)
            attribute(attrs, "syntwine.content_base64", *build.content_base64);
        if (build.content_omitted_bytes)
            attribute(attrs, "syntwine.content_omitted_bytes", build.content_omitted_bytes);
        attribute(attrs, "syntwine.dropped_events", build.dropped_events);
        attribute(attrs, "syntwine.sink_dropped_events", build.sink_dropped_events);
    }
    span["attributes"] = std::move(attrs);
    return span;
}

Json
otlp_log(const Build& build, const Record& record) {
    auto log = object();
    log["traceId"] = build.trace_id;
    if (record.parent)
        log["spanId"] = hex_id(record.parent);
    log["timeUnixNano"] = std::to_string(record.start);
    log["severityNumber"] = record.error_code.empty() ? 9 : 17;
    auto body = object();
    body["stringValue"] = record.name;
    log["body"] = std::move(body);
    log["attributes"] = otlp_attributes(build, record);
    return log;
}

Json
otlp_records(const Batch& batch, Signal signal, std::vector<Record> Build::* field, auto encode) {
    auto records = array();
    for (const auto& build : batch) {
        if (!pending(build, signal))
            continue;
        for (const auto& record : (*build).*field)
            records.push_back(encode(*build, record));
    }
    return records;
}

Json
otlp_metrics(const Batch& batch, std::uint64_t& points_count) {
    std::map<std::string, Json> points;
    for (const auto& build : batch) {
        if (!pending(build, Signal::metrics))
            continue;
        const auto values = metrics_json(*build);
        for (const auto& item : values.object_range()) {
            ++points_count;
            auto point = object();
            point["timeUnixNano"] = std::to_string(build->start + build->duration);
            const auto value = std::to_string(item.value().as<std::uint64_t>());
            point["asInt"] = value;
            if (!build->spans.empty()) {
                auto exemplar = object();
                exemplar["timeUnixNano"] = std::to_string(build->start + build->duration);
                exemplar["asInt"] = value;
                exemplar["traceId"] = build->trace_id;
                exemplar["spanId"] = hex_id(build->spans.front().id);
                auto exemplars = array();
                exemplars.push_back(std::move(exemplar));
                point["exemplars"] = std::move(exemplars);
            }
            auto [output, _] = points.try_emplace(std::string(item.key()), array());
            output->second.push_back(std::move(point));
        }
    }
    auto metrics = array();
    for (auto& [name, data] : points) {
        auto metric = object();
        metric["name"] = "syntwine." + name;
        metric["unit"] = name.ends_with("_ns") ? "ns" : name == "output_bytes" ? "By" : "1";
        auto gauge = object();
        gauge["dataPoints"] = std::move(data);
        metric["gauge"] = std::move(gauge);
        metrics.push_back(std::move(metric));
    }
    return metrics;
}

std::string
otlp_envelope(
    std::string_view resource_key,
    std::string_view scope_key,
    std::string_view signal_key,
    Json payload
) {
    auto scope = object();
    scope["name"] = "syntwine";
    auto scoped_payload = object();
    scoped_payload["scope"] = std::move(scope);
    scoped_payload[signal_key] = std::move(payload);
    auto scopes = array();
    scopes.push_back(std::move(scoped_payload));

    auto attrs = array();
    attribute(attrs, "service.name", "syntwine");
    auto resource = object();
    resource["attributes"] = std::move(attrs);
    auto resource_payload = object();
    resource_payload["resource"] = std::move(resource);
    resource_payload[scope_key] = std::move(scopes);
    auto resources = array();
    resources.push_back(std::move(resource_payload));

    auto request = object();
    request[resource_key] = std::move(resources);
    return dump(request);
}

struct Response {
    long code = 0;
    std::string body;
    bool truncated = false;
};

size_t
receive_response(char* bytes, size_t size, size_t count, void* storage) noexcept {
    constexpr std::size_t limit = 65536;
    auto& response = *static_cast<Response*>(storage);
    const auto available = size * count;
    const auto retained = std::min(available, limit - response.body.size());
    try {
        response.body.append(bytes, retained);
        response.truncated |= retained < available;
        return available;
    } catch (...) {
        return 0;
    }
}

using CurlHeaders = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

void
append_header(CurlHeaders& headers, const std::string& header) {
    auto* list = curl_slist_append(headers.get(), header.c_str());
    if (!list)
        throw std::bad_alloc();
    headers.release();
    headers.reset(list);
}

void
check_curl(CURLcode status) {
    if (status != CURLE_OK)
        throw std::runtime_error(std::string("OTLP telemetry: ") + curl_easy_strerror(status));
}

#ifdef __linux__
const char*
environment_path(const char* name) {
    const auto* path = std::getenv(name);
    return path && *path ? path : nullptr;
}

const char*
linux_ca_bundle() {
    if (const auto* bundle = environment_path("CURL_CA_BUNDLE"))
        return bundle;
    if (const auto* bundle = environment_path("SSL_CERT_FILE"))
        return bundle;
    // A manylinux build image need not use the same trust-store path as its caller.
    constexpr const char* system_bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/ssl/cert.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    };
    for (const auto* path : system_bundles) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
            return path;
    }
    return nullptr;
}
#endif

class Http {
public:
    Http(const OTLPSink::Headers& headers, double timeout)
        : headers_(nullptr, curl_slist_free_all),
          handle_(make_handle(), curl_easy_cleanup) {
        if (!handle_)
            throw std::runtime_error("OTLP telemetry: cannot allocate HTTP client");
        append_header(headers_, "Content-Type: application/json");
        for (const auto& [key, value] : headers)
            append_header(headers_, key + ": " + value);
        set(CURLOPT_HTTPHEADER, headers_.get());
        set(CURLOPT_POST, 1L);
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_TIMEOUT_MS, static_cast<long>(std::ceil(timeout * 1000)));
        set(CURLOPT_WRITEFUNCTION, &receive_response);
        set(CURLOPT_ERRORBUFFER, error_.data());
#ifdef __linux__
        if (const auto* bundle = linux_ca_bundle()) {
            set(CURLOPT_CAINFO, bundle);
            set(CURLOPT_PROXY_CAINFO, bundle);
        }
        if (const auto* directory = environment_path("SSL_CERT_DIR")) {
            set(CURLOPT_CAPATH, directory);
            set(CURLOPT_PROXY_CAPATH, directory);
        }
#endif
    }

    Response
    send(const std::string& url, const std::string& payload) {
        Response response;
        set(CURLOPT_URL, url.c_str());
        set(CURLOPT_POSTFIELDS, payload.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
        set(CURLOPT_WRITEDATA, &response);
        error_.front() = '\0';
        if (const auto result = curl_easy_perform(handle_.get()); result != CURLE_OK) {
            const auto* detail = error_.front() ? error_.data() : curl_easy_strerror(result);
            throw std::runtime_error("OTLP telemetry: " + url + ": " + detail);
        }
        check_curl(curl_easy_getinfo(handle_.get(), CURLINFO_RESPONSE_CODE, &response.code));
        return response;
    }

private:
    static CURL*
    make_handle() {
        struct CurlGlobal {
            CurlGlobal() {
                check_curl(curl_global_init(CURL_GLOBAL_DEFAULT));
            }

            ~CurlGlobal() {
                curl_global_cleanup();
            }
        };

        static const CurlGlobal global;
        return curl_easy_init();
    }

    template <class T>
    void
    set(CURLoption option, T value) {
        check_curl(curl_easy_setopt(handle_.get(), option, value));
    }

    CurlHeaders headers_;
    std::array<char, CURL_ERROR_SIZE> error_{};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle_;
};

std::string
signal_url(std::string_view endpoint, std::string_view signal) {
    const auto query = endpoint.find('?');
    auto base = endpoint.substr(0, query);
    while (base.ends_with('/'))
        base.remove_suffix(1);
    auto url = std::string(base) + "/v1/" + std::string(signal);
    if (query != endpoint.npos)
        url += endpoint.substr(query);
    return url;
}

bool
valid_header(std::string_view name, std::string_view value) {
    constexpr std::string_view punctuation = "!#$%&'*+-.^`_|~";
    if (name.empty() || value.find_first_of("\r\n\0", 0, 3) != value.npos)
        return false;
    for (char c : name) {
        if ((c >= '0' && c <= '9')
            || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || punctuation.find(c) != punctuation.npos)
            continue;
        return false;
    }
    return true;
}

struct Wire {
    Signal kind;
    std::string_view route, resource, scope, items, rejected;
};

constexpr Wire
    trace_signal{Signal::traces, "traces", "resourceSpans", "scopeSpans", "spans", "rejectedSpans"};
constexpr Wire metric_signal{
    Signal::metrics,
    "metrics",
    "resourceMetrics",
    "scopeMetrics",
    "metrics",
    "rejectedDataPoints"
};
constexpr Wire log_signal{
    Signal::logs,
    "logs",
    "resourceLogs",
    "scopeLogs",
    "logRecords",
    "rejectedLogRecords"
};

struct Acknowledgement {
    std::uint64_t rejected = 0;
    std::string warning;
    bool failed = false;
};

Acknowledgement
acknowledgement(
    const Response& response,
    const Wire& wire,
    std::uint64_t sent,
    const std::string& url
) {
    if (response.code < 200 || response.code >= 300) {
        auto message = "OTLP telemetry: " + url + " returned HTTP " + std::to_string(response.code);
        // The OTLP protocol lists only these HTTP statuses as retryable.
        if (response.code == 429
            || response.code == 502
            || response.code == 503
            || response.code == 504)
            throw std::runtime_error(std::move(message));
        return {sent, std::move(message), true};
    }
    if (response.truncated)
        return {0, "OTLP telemetry: acknowledgement body exceeded 64 KiB for " + url};
    if (response.body.empty())
        return {};
    try {
        const auto body = Json::parse(response.body);
        if (!body.contains("partialSuccess"))
            return {};
        const auto& partial = body.at("partialSuccess");
        const auto count =
            partial.contains(wire.rejected) ? partial.at(wire.rejected).as<std::uint64_t>() : 0;
        const auto message =
            partial.contains("errorMessage") ? partial.at("errorMessage").as<std::string>() : "";
        if (count)
            return {
                count,
                "OTLP telemetry: "
                    + url
                    + " rejected "
                    + std::to_string(count)
                    + " records"
                    + (message.empty() ? "" : ": " + message),
                true
            };
        if (!message.empty())
            return {0, "OTLP telemetry: " + url + ": " + message};
    } catch (const jsoncons::json_exception&) {
        return {0, "OTLP telemetry: invalid JSON acknowledgement from " + url};
    }
    return {};
}
} // namespace

OTLPSink::OTLPSink(std::string endpoint, Headers headers, double timeout, std::size_t capacity)
    : Sink(capacity),
      endpoint_(std::move(endpoint)),
      headers_(std::move(headers)),
      timeout_(timeout) {
    const auto sep = endpoint_.find("://");
    if ((!endpoint_.starts_with("http://") && !endpoint_.starts_with("https://"))
        || endpoint_.find_first_of("#\r\n\0", 0, 4) != endpoint_.npos
        || endpoint_.find_first_not_of('/', sep + 3) == endpoint_.npos)
        throw std::invalid_argument("OTLP endpoint must be a base HTTP(S) URL");
    if (!std::isfinite(timeout)
        || timeout <= 0
        || std::ceil(timeout * 1000) >= static_cast<double>(std::numeric_limits<long>::max()))
        throw std::invalid_argument("OTLP timeout must be a positive finite number in seconds");
    for (const auto& [name, value] : headers_) {
        if (!valid_header(name, value))
            throw std::invalid_argument("invalid OTLP HTTP header");
    }
}

std::optional<std::string>
OTLPSink::last_export_warning() const {
    const std::lock_guard guard(warning_mutex_);
    return warning_;
}

void
OTLPSink::export_batch(Batch& batch) {
    auto spans = otlp_records(batch, Signal::traces, &Build::spans, otlp_span);
    auto logs = otlp_records(batch, Signal::logs, &Build::events, otlp_log);
    std::uint64_t points = 0;
    auto metrics = otlp_metrics(batch, points);
    if (spans.empty() && metrics.empty() && logs.empty())
        return;

    Http client(headers_, timeout_);
    std::string failure;
    auto send = [&](const Wire& wire, Json body, std::uint64_t count) {
        if (body.empty()) {
            acknowledge(batch, wire.kind);
            return;
        }
        const auto url = signal_url(endpoint_, wire.route);
        const auto response =
            client.send(url, otlp_envelope(wire.resource, wire.scope, wire.items, std::move(body)));
        auto result = acknowledgement(response, wire, count, url);
        acknowledge(batch, wire.kind);
        if (result.rejected)
            rejected_.fetch_add(result.rejected, std::memory_order_relaxed);
        if (!result.warning.empty()) {
            const std::lock_guard guard(warning_mutex_);
            warning_ = result.warning;
        }
        if (result.failed && failure.empty())
            failure = std::move(result.warning);
    };
    const auto span_count = spans.size();
    const auto log_count = logs.size();
    send(trace_signal, std::move(spans), span_count);
    send(metric_signal, std::move(metrics), points);
    send(log_signal, std::move(logs), log_count);
    if (!failure.empty())
        throw std::runtime_error(std::move(failure));
}
} // namespace syntwine::telemetry
