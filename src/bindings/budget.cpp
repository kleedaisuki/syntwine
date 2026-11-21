#include "syntwine/detail/core/budget.hpp"

#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace syntwine {

void bind_budget(pybind11::module_& module) {
    namespace py = pybind11;
    using py::arg;
    py::class_<Budget>(module, "Budget")
        .def(py::init<std::optional<std::size_t>, std::optional<std::size_t>, std::optional<std::size_t>>(),
             py::kw_only(), arg("characters") = py::none(), arg("bytes") = py::none(),
             arg("tokens") = py::none())
        .def_readonly("characters", &Budget::characters)
        .def_readonly("bytes", &Budget::bytes)
        .def_readonly("tokens", &Budget::tokens);
    py::class_<Usage>(module, "Usage")
        .def_readonly("characters", &Usage::characters)
        .def_readonly("bytes", &Usage::bytes)
        .def_readonly("tokens", &Usage::tokens);
    module.def("_set_tokenizer_data_dir", &set_tokenizer_data_dir);
}

} // namespace syntwine
