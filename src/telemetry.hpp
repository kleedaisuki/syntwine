#pragma once

#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace syntwine::telemetry {

enum class Level : unsigned { build, stages, operations };

class Trace {
public:
    struct State;

    explicit Trace(std::unique_ptr<State>);
    ~Trace();
    Trace(const Trace&) = delete;
    Trace& operator=(const Trace&) = delete;

    const std::string& id() const noexcept;
    bool records_dependencies() const noexcept;
    void dependency(std::uint64_t parent, std::uint64_t child, bool discovered) noexcept;
    void event(std::string_view name, std::uint64_t node_id = 0,
               std::string_view source_id = {}, std::string_view operation = {}) noexcept;
    void finish(std::string_view error_code = {},
                std::optional<std::string_view> content = std::nullopt) noexcept;
    pybind11::dict view() const;

private:
    friend class Span;
    std::unique_ptr<State> state_;
};

class Span {
public:
    Span(Trace* trace, std::string_view name, Level level = Level::operations,
         std::uint64_t node_id = 0, std::string_view source_id = {},
         std::string_view operation = {}) noexcept : trace_(trace) {
        if (trace_)
            begin(name, level, node_id, source_id, operation);
    }
    ~Span() {
        if (trace_)
            end();
    }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    void fail(std::string_view code) noexcept;
    void input_bytes(std::optional<std::size_t> value) noexcept;
    void output_bytes(std::optional<std::size_t> value) noexcept;
    void characters(std::optional<std::size_t> value) noexcept;
    void tokens(std::optional<std::size_t> value) noexcept;
    void cache_hit(bool value = true) noexcept;
    void discovered_dependencies(std::size_t value) noexcept;

private:
    enum class Metric { none, native, python, token, node };
    static constexpr std::size_t no_record = static_cast<std::size_t>(-1);

    void begin(std::string_view name, Level level, std::uint64_t node_id,
               std::string_view source_id, std::string_view operation) noexcept;
    void end() noexcept;

    Trace* trace_;
    std::size_t record_ = no_record;
    std::size_t previous_ = no_record;
    std::uint64_t start_ = 0;
    int exceptions_ = 0;
    Metric metric_ = Metric::none;
    bool cached_ = false;
    bool output_stage_ = false;
};

std::unique_ptr<Trace> start(pybind11::handle config, std::uint64_t node_id = 0,
                             std::string_view source_id = {}, std::string_view operation = {});
void bind(pybind11::module_& module);

} // namespace syntwine::telemetry
