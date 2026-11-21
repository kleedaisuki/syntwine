#include "budget.hpp"
#include "core.hpp"
#include "document.hpp"
#include "json.hpp"
#include "markdown.hpp"
#include "telemetry.hpp"
#include "text.hpp"
#include "toml.hpp"
#include "xml.hpp"

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
