#pragma once

#include "syntwine/detail/telemetry/telemetry.hpp"

#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace syntwine::telemetry {

inline constexpr std::size_t default_capacity = 65536;
inline constexpr std::size_t default_max_events = 16384;

struct Record {
    std::string name, source_id, operation, error_code;
    std::uint64_t id = 0, parent = 0, node = 0, start = 0, duration = 0;
    std::optional<std::size_t> input, output, characters, tokens, discovered_dependencies;
    std::optional<bool> cache_hit;
    std::uint64_t dependency_node = 0;
};

struct Edge {
    std::uint64_t parent, child;
    bool discovered;
};

struct Metrics {
    std::uint64_t cache_hits = 0, nodes = 0, native_ns = 0, python_ns = 0, token_ns = 0;
};

struct Build {
    std::string trace_id, build_id, error_code;
    std::uint64_t start = 0, duration = 0;
    std::optional<std::size_t> output_bytes;
    std::optional<std::string> content_base64;
    std::size_t content_omitted_bytes = 0, dropped_events = 0, sink_dropped_events = 0;
    std::vector<Record> spans, events;
    std::vector<Edge> dependencies;
    std::optional<Metrics> metrics;

    std::size_t weight() const noexcept {
        return 1 + spans.size() - !spans.empty() + events.size() + dependencies.size();
    }
};

std::string hex_id(std::uint64_t value);
pybind11::dict build_python(const Build& build);

enum class Signal : unsigned { traces = 1, metrics = 2, logs = 4 };

struct QueuedBuild {
    std::shared_ptr<const Build> build;
    unsigned pending_signals = 7;

    const Build* operator->() const noexcept { return build.get(); }
    const Build& operator*() const noexcept { return *build; }
};

using Batch = std::deque<QueuedBuild>;

class Sink : public std::enable_shared_from_this<Sink> {
public:
    explicit Sink(std::size_t capacity);
    virtual ~Sink() = default;

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t dropped_events() const;
    void submit(std::shared_ptr<Build> build);
    void flush();
    Batch snapshot() const;
    void clear();

protected:
    virtual bool retains_records() const noexcept { return false; }
    virtual void export_batch(Batch&) = 0;

private:
    void evict();
    void track();
    void untrack();

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::mutex flush_mutex_;
    Batch queue_;
    std::size_t weight_ = 0, dropped_ = 0;
    bool tracked_ = false;
};

class MemorySink final : public Sink {
public:
    using Sink::Sink;

private:
    bool retains_records() const noexcept override { return true; }
    void export_batch(Batch&) override {}
};

class JSONLSink final : public Sink {
public:
    JSONLSink(std::filesystem::path path, std::size_t capacity)
        : Sink(capacity), path_(std::move(path)) {}
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void export_batch(Batch& batch) override;
    std::filesystem::path path_;
};

class SQLiteSink final : public Sink {
public:
    SQLiteSink(std::filesystem::path path, std::size_t capacity)
        : Sink(capacity), path_(std::move(path)) {}
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void export_batch(Batch& batch) override;
    std::filesystem::path path_;
};

class OTLPSink final : public Sink {
public:
    using Headers = std::map<std::string, std::string>;
    OTLPSink(std::string endpoint, Headers headers, double timeout, std::size_t capacity);
    const std::string& endpoint() const noexcept { return endpoint_; }
    double timeout() const noexcept { return timeout_; }
    std::uint64_t rejected_records() const noexcept { return rejected_.load(std::memory_order_relaxed); }
    std::optional<std::string> last_export_warning() const;

private:
    void export_batch(Batch& batch) override;
    std::string endpoint_;
    Headers headers_;
    double timeout_;
    std::atomic<std::uint64_t> rejected_ = 0;
    mutable std::mutex warning_mutex_;
    std::optional<std::string> warning_;
};

void flush_sinks();

} // namespace syntwine::telemetry
