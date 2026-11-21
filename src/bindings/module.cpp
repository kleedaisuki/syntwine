#include "syntwine/detail/core/budget.hpp"
#include "syntwine/detail/core/core.hpp"
#include "syntwine/detail/core/document.hpp"
#include "syntwine/detail/formats/json.hpp"
#include "syntwine/detail/formats/markdown.hpp"
#include "syntwine/detail/telemetry/telemetry.hpp"
#include "syntwine/detail/formats/text.hpp"
#include "syntwine/detail/formats/toml.hpp"
#include "syntwine/detail/formats/xml.hpp"

PYBIND11_MODULE(_syntwine_native, module) {
    using namespace syntwine;
    module.doc() = "Native Syntwine expression graph and evaluation session";
    bind_budget(module);
    bind_core(module);
    bind_text(module);
    bind_markdown(module);
    bind_json(module);
    bind_toml(module);
    bind_xml(module);
    bind_document(module);
    telemetry::bind(module);
    bind_source_aliases(module);
}
