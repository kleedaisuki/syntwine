#include "syntwine/detail/formats/toml.hpp"
#include "syntwine/detail/formats/json.hpp"
#include "syntwine/detail/platform/utf8.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace syntwine {
namespace {

using Path = std::vector<std::string>;

std::string
position(toml::source_position pos) {
    return std::to_string(pos.line) + ":" + std::to_string(pos.column);
}

std::string
format_value(const toml::node& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    if (const auto* table = value.as_table()) {
        auto in = *table;
        in.is_inline(true);
        out << toml::toml_formatter(in);
    } else {
        out << toml::toml_formatter(value);
    }
    return std::move(out).str();
}

std::string
format_table(const toml::table& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    if (value.is_inline()) {
        auto expanded = value;
        expanded.is_inline(false);
        out << toml::toml_formatter(expanded);
    } else {
        out << toml::toml_formatter(value);
    }
    return std::move(out).str();
}

std::string
format_key(std::string_view key) {
    constexpr std::string_view bare =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    if (!key.empty() && key.find_first_not_of(bare) == std::string_view::npos)
        return std::string(key);
    std::ostringstream out;
    out << toml::toml_formatter(toml::value(std::string(key)), toml::format_flags::none);
    return std::move(out).str();
}

std::string
format_path(const Path& path, std::size_t first = 0) {
    std::string result;
    for (std::size_t i = first; i < path.size(); ++i) {
        if (i != first)
            result += '.';
        result += format_key(path[i]);
    }
    return result;
}

Path
parse_path(std::string_view path) {
    // A string with no TOML path punctuation is an ordinary single key, including spaces.
    if (path.find_first_of(".'\"") == std::string_view::npos)
        return {std::string(path)};
    try {
        auto parsed = toml::parse(std::string("x = {") + std::string(path) + " = 0}");
        const auto* node = parsed.get("x");
        Path out;
        while (node && node->is_table()) {
            const auto& table = *node->as_table();
            if (table.size() != 1)
                break;
            const auto [key, child] = *table.begin();
            out.emplace_back(key.str());
            node = &child;
        }
        if (parsed.size() == 1
            && node
            && node->is_integer()
            && node->value_or(-1) == 0
            && !out.empty())
            return out;
    } catch (const toml::parse_error&) {}
    throw Error(ErrorCode::type, "invalid TOML key path: " + std::string(path));
}

Path
join_path(Path base, const Path& suffix) {
    base.insert(base.end(), suffix.begin(), suffix.end());
    return base;
}

const toml::node*
lookup(const toml::table& root, const Path& path) {
    const toml::node* node = &root;
    for (const auto& key : path) {
        const auto* tab = node->as_table();
        if (!tab)
            return nullptr;
        node = tab->get(key);
        if (!node)
            return nullptr;
    }
    return node;
}

toml::table*
ensure_table(toml::table& root, const Path& path, bool create) {
    auto* tab = &root;
    for (const auto& key : path) {
        auto* child = tab->get(key);
        if (!child && create) {
            const auto [it, inserted] = tab->insert(key, toml::table{});
            child = &it->second;
        }
        if (!child)
            return nullptr;
        tab = child->as_table();
        if (!tab)
            throw Error(ErrorCode::type, "TOML path crosses a non-table value: " + key);
    }
    return tab;
}

struct TomlSource {
    TomlSource(toml::table data, std::string text)
        : tree(std::move(data)),
          source(std::move(text)) {}

    toml::table tree;
    std::string source;
};

using TomlPtr = std::shared_ptr<const TomlSource>;

TomlPtr
read_source(std::string_view text) {
    try {
        return std::make_shared<const TomlSource>(toml::parse(text), std::string(text));
    } catch (const toml::parse_error& e) {
        throw Error(ErrorCode::parse, std::string(e.description()), position(e.source().begin));
    }
}

TomlPtr
canonical_source(const toml::table& table) {
    auto encoded = format_table(table);
    try {
        return read_source(encoded);
    } catch (const Error& error) {
        throw Error(ErrorCode::serialization, error.what(), error.location);
    }
}

std::vector<std::string_view>
comments(std::string_view text) {
    std::vector<std::string_view> result;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto ch = text[pos];
        if (ch == '#') {
            const auto end = text.find_first_of("\r\n", pos);
            result.push_back(text.substr(pos, end == std::string_view::npos ? end : end - pos));
            pos = end == std::string_view::npos ? text.size() : end;
        } else if (ch == '\'' || ch == '"') {
            const auto is_triple =
                pos + 2 < text.size() && text[pos + 1] == ch && text[pos + 2] == ch;
            pos += is_triple ? 3 : 1;
            while (pos < text.size()) {
                if (ch == '"' && text[pos] == '\\') {
                    pos += 2;
                    continue;
                }
                if (text[pos++] != ch)
                    continue;
                if (!is_triple)
                    break;
                if (pos + 1 >= text.size() || text[pos] != ch || text[pos + 1] != ch)
                    continue;
                pos += 2;
                while (pos < text.size() && text[pos] == ch)
                    ++pos;
                break;
            }
        } else {
            ++pos;
        }
    }
    return result;
}

struct Edit {
    std::size_t start, end;
    std::string text;
};

struct Header {
    toml::source_region location;
    Path path;
};

bool
has_prefix(const Path& path, const Path& prefix) {
    return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

class Layout {
public:
    explicit Layout(std::string_view text, const toml::table& tree)
        : text_(text),
          starts_{0} {
        for (std::size_t i = 0; i < text_.size(); ++i)
            if (text_[i] == '\n')
                starts_.push_back(i + 1);
        collect_headers(tree, {});
        std::ranges::sort(headers_, [](const Header& a, const Header& b) {
            return a.location.begin < b.location.begin;
        });
    }

    const std::vector<Header>&
    headers() const {
        return headers_;
    }

    std::string_view
    newline() const {
        const auto first = text_.find('\n');
        return first != std::string_view::npos && first && text_[first - 1] == '\r' ? "\r\n" : "\n";
    }

    std::string_view
    source() const {
        return text_;
    }

    std::size_t
    offset(toml::source_position pos) const {
        if (!pos.line || pos.line > starts_.size())
            return text_.size();
        auto byte = starts_[pos.line - 1];
        if (pos.line == 1 && text_.starts_with("\xef\xbb\xbf"))
            byte += 3;
        // toml++ columns count Unicode code points rather than UTF-8 bytes.
        for (std::size_t col = 1; col < pos.column && byte < text_.size(); ++col) {
            ++byte;
            while (byte < text_.size() && (static_cast<unsigned char>(text_[byte]) & 0xc0) == 0x80)
                ++byte;
        }
        return byte;
    }

    std::size_t
    line_start(std::size_t pos) const {
        return *std::prev(std::ranges::upper_bound(starts_, pos));
    }

    std::size_t
    line_end(std::size_t pos) const {
        const auto it = std::ranges::upper_bound(starts_, pos);
        return it == starts_.end() ? text_.size() : *it;
    }

    bool
    header(const toml::table& table) const {
        if (table.is_inline() || &table == root_)
            return false;
        const auto begin = offset(table.source().begin);
        return begin < text_.size() && text_[begin] == '[';
    }

    bool
    array_of_tables(const toml::array& array) const {
        return text_.substr(offset(array.source().begin)).starts_with("[[");
    }

    std::vector<Edit>
    header_keys(const Header& header) const {
        std::vector<Edit> keys;
        auto pos = offset(header.location.begin);
        const auto end = offset(header.location.end);
        while (pos < end) {
            if (std::string_view{"[]. \t"}.find(text_[pos]) != std::string_view::npos) {
                ++pos;
                continue;
            }
            const auto start = pos;
            const auto quote = text_[pos];
            if (quote == '"' || quote == '\'') {
                ++pos;
                while (pos < end) {
                    const auto c = text_[pos++];
                    if (c == quote)
                        break;
                    if (quote == '"' && c == '\\' && pos < end)
                        ++pos;
                }
            } else {
                pos = text_.find_first_of(".] \t", pos);
                if (pos == std::string_view::npos || pos > end)
                    pos = end;
            }
            keys.push_back({start, pos, {}});
        }
        return keys;
    }

    Edit
    remove_statement(toml::source_region where) const {
        const auto start = line_start(offset(where.begin));
        const auto end = line_end(offset(where.end));
        std::string comment_lines;
        for (const auto comment : comments(text_.substr(start, end - start))) {
            comment_lines += comment;
            comment_lines += newline();
        }
        return {start, end, std::move(comment_lines)};
    }

    Edit
    replace_value(toml::source_region where, std::string value) const {
        const auto start = offset(where.begin);
        const auto end = offset(where.end);
        const auto embedded = comments(text_.substr(start, end - start));
        if (embedded.empty())
            return {start, end, std::move(value)};
        // Put annotations for removed array entries above the assignment, never inside strings.
        const auto line = line_start(start);
        std::string replacement;
        for (const auto comment : embedded) {
            replacement += comment;
            replacement += newline();
        }
        replacement += text_.substr(line, start - line);
        replacement += value;
        return {line, end, std::move(replacement)};
    }

    Edit
    add(const Path& path, const toml::table& original, const toml::node& value) const {
        std::size_t prefix = path.size() - 1;
        std::size_t own_header = 0;
        while (prefix != 0) {
            const Path ancestor(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(prefix));
            if (const auto* node = lookup(original, ancestor)) {
                if (const auto* table = node->as_table(); table && header(*table)) {
                    own_header = offset(table->source().begin);
                    break;
                }
            }
            --prefix;
        }

        auto where = text_.size();
        for (const auto& next : headers_) {
            const auto start = offset(next.location.begin);
            if (prefix == 0 || start > own_header) {
                where = line_start(start);
                break;
            }
        }
        const auto boundary = prefix == 0 ? 0 : line_end(own_header);
        // Keep the comments and blank lines that introduce the following section together.
        if (where != text_.size()) {
            while (where > boundary) {
                const auto previous = line_start(where - 1);
                auto trim = text_.substr(previous, where - previous);
                const auto non_space = trim.find_first_not_of(" \t\r\n");
                if (non_space != std::string_view::npos && trim[non_space] != '#')
                    break;
                if (prefix == 0 && previous == 0 && non_space != std::string_view::npos)
                    break;
                where = previous;
            }
        }
        std::string out;
        if (where && text_[where - 1] != '\n')
            out += newline();
        out += format_path(path, prefix);
        out += " = ";
        out += format_value(value);
        out += newline();
        return {where, where, std::move(out)};
    }

private:
    void
    collect_headers(const toml::table& tree, const Path& path) {
        if (path.empty())
            root_ = &tree;
        if (header(tree))
            headers_.push_back({tree.source(), path});
        if (tree.is_inline())
            return;
        for (const auto& [key, value] : tree) {
            auto child_path = path;
            child_path.emplace_back(key.str());
            if (const auto* table = value.as_table())
                collect_headers(*table, child_path);
            else if (const auto* array = value.as_array(); array && array_of_tables(*array))
                for (const auto& entry : *array)
                    if (const auto* entry_table = entry.as_table())
                        collect_headers(*entry_table, child_path);
        }
    }

    std::string_view text_;
    std::vector<std::size_t> starts_;
    const toml::table* root_ = nullptr;
    std::vector<Header> headers_;
};

void
plan_remove(const Layout& layout, const toml::node& value, std::vector<Edit>& edits) {
    if (const auto* table = value.as_table(); table && !table->is_inline()) {
        if (layout.header(*table))
            edits.push_back(layout.remove_statement(table->source()));
        for (const auto& [_, node] : *table)
            plan_remove(layout, node, edits);
    } else if (const auto* array = value.as_array(); array && layout.array_of_tables(*array)) {
        for (const auto& item : *array)
            plan_remove(layout, item, edits);
    } else {
        edits.push_back(layout.remove_statement(value.source()));
    }
}

bool
equal(const toml::node& lhs, const toml::node& rhs) {
    if (lhs.type() != rhs.type())
        return false;
    return lhs.visit([&](const auto& concrete) {
        using T = std::remove_cvref_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, toml::table>) {
            const auto& other = *rhs.as_table();
            if (concrete.size() != other.size())
                return false;
            for (const auto& [key, value] : concrete) {
                const auto* item = other.get(key.str());
                if (!item || !equal(value, *item))
                    return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, toml::array>) {
            return std::ranges::equal(concrete, *rhs.as_array(), equal);
        } else if constexpr (std::is_same_v<T, toml::value<double>>) {
            // Keep the sign of zero; the TOML spec gives NaN signs no distinct semantics.
            const double a = concrete.get(), b = rhs.ref<double>();
            return (a == b && std::signbit(a) == std::signbit(b))
                   || (std::isnan(a) && std::isnan(b));
        } else {
            return concrete == *rhs.as<typename T::value_type>();
        }
    });
}

void
plan_diff(
    const Layout& layout,
    const toml::table& before,
    const toml::table& after,
    const toml::table& root,
    const Path& parent,
    std::vector<Edit>& edits
) {
    for (const auto& [key, old] : before) {
        const auto* current = after.get(key.str());
        if (!current) {
            plan_remove(layout, old, edits);
            continue;
        }
        if (equal(old, *current))
            continue;
        auto path = parent;
        path.emplace_back(key.str());
        const auto* old_table = old.as_table();
        const auto* new_table = current->as_table();
        if (old_table && new_table && !old_table->is_inline()) {
            plan_diff(layout, *old_table, *new_table, root, path, edits);
            if (new_table->empty() && !layout.header(*old_table))
                edits.push_back(layout.add(path, root, *current));
        } else if (
            (old_table && !old_table->is_inline())
            || (old.as_array() && layout.array_of_tables(*old.as_array()))
        ) {
            plan_remove(layout, old, edits);
            edits.push_back(layout.add(path, root, *current));
        } else {
            edits.push_back(layout.replace_value(old.source(), format_value(*current)));
        }
    }
    for (const auto& [key, value] : after) {
        if (before.contains(key.str()))
            continue;
        auto path = parent;
        path.emplace_back(key.str());
        edits.push_back(layout.add(path, root, value));
    }
}

std::optional<std::string>
apply(const Layout& layout, std::vector<Edit>& edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) {
        return std::pair{a.start, a.end} < std::pair{b.start, b.end};
    });
    std::string result;
    std::size_t cursor = 0;
    for (const auto& e : edits) {
        if (e.start < cursor || e.end > layout.source().size())
            return std::nullopt;
        result += layout.source().substr(cursor, e.start - cursor);
        result += e.text;
        cursor = e.end;
    }
    result += layout.source().substr(cursor);
    return result;
}

TomlPtr
try_edits(const Layout& layout, const toml::table& expected, std::vector<Edit> edits) {
    if (const auto patched = apply(layout, edits)) {
        try {
            auto root = read_source(*patched);
            if (equal(root->tree, expected))
                return root;
        } catch (const Error& e) {
            if (e.code != ErrorCode::parse)
                throw;
        }
    }
    return {};
}

TomlPtr
reconcile(const TomlPtr& previous, const toml::table& updated) {
    py::gil_scoped_release release;
    if (equal(previous->tree, updated))
        return previous;
    const Layout layout(previous->source, previous->tree);
    std::vector<Edit> edits;
    plan_diff(layout, previous->tree, updated, previous->tree, {}, edits);
    if (auto patched = try_edits(layout, updated, std::move(edits)))
        return patched;

    // Some structural changes cannot keep every token (e.g. removing the only dotted
    // declaration of an implicit table). Retain source comments even on canonical fallback.
    std::string output;
    for (const auto comment : comments(previous->source)) {
        output += comment;
        output += layout.newline();
    }
    auto canonical = format_table(updated);
    if (layout.newline() == "\n")
        output += canonical;
    else
        for (const auto ch : canonical) {
            if (ch == '\n')
                output += '\r';
            output += ch;
        }
    try {
        auto result = read_source(output);
        if (!equal(result->tree, updated))
            throw Error(ErrorCode::serialization, "TOML serialization changed a value");
        return result;
    } catch (const Error& e) {
        if (e.code != ErrorCode::parse)
            throw;
        throw Error(ErrorCode::serialization, e.what(), e.location);
    }
}

const toml::table&
selected_table(const TomlSource& root, const Path& path) {
    const auto* node = lookup(root.tree, path);
    const auto* table = node ? node->as_table() : nullptr;
    if (!table)
        throw Error(ErrorCode::type, "TOML table does not exist: " + format_path(path));
    return *table;
}

std::optional<std::string>
view_source(const TomlSource& root, const Path& scope, const toml::table& selected) {
    const Layout layout(root.source, root.tree);
    if (!layout.header(selected))
        return std::nullopt;
    const auto& headers = layout.headers();
    std::string contents, descendants;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!has_prefix(header.path, scope))
            continue;
        const auto start = layout.offset(header.location.begin);
        const auto body = layout.line_end(layout.offset(header.location.end));
        const auto end = i + 1 == headers.size()
                             ? root.source.size()
                             : layout.line_start(layout.offset(headers[i + 1].location.begin));
        if (header.path == scope) {
            const auto after = layout.offset(header.location.end);
            const auto note = layout.source().substr(after, body - after);
            if (const auto hash = note.find('#'); hash != std::string_view::npos)
                contents += note.substr(hash);
            contents += layout.source().substr(body, end - body);
        } else {
            const auto keys = layout.header_keys(header);
            if (keys.size() != header.path.size())
                return std::nullopt;
            descendants += layout.source().substr(start, keys.front().start - start);
            descendants +=
                layout.source().substr(keys[scope.size()].start, end - keys[scope.size()].start);
        }
    }
    if (!contents.empty() && !descendants.empty() && contents.back() != '\n')
        contents += layout.newline();
    contents += descendants;
    try {
        if (equal(toml::parse(contents), selected))
            return contents;
    } catch (const toml::parse_error&) {}
    return std::nullopt;
}

template <class T>
std::string
time_string(const T& value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return std::move(out).str();
}

py::object
toml_to_python(const toml::node& node) {
    return node.visit([](const auto& element) -> py::object {
        using T = std::remove_cvref_t<decltype(element)>;
        if constexpr (std::is_same_v<T, toml::table>) {
            py::dict result;
            for (const auto& [key, item] : element)
                result[py::str(key.str())] = toml_to_python(item);
            return result;
        } else if constexpr (std::is_same_v<T, toml::array>) {
            py::list result;
            for (const auto& item : element)
                result.append(toml_to_python(item));
            return result;
        } else if constexpr (std::is_same_v<T, toml::value<toml::date_time>>) {
            return py::module_::import("datetime")
                .attr("datetime")
                .attr("fromisoformat")(time_string(element.get()));
        } else if constexpr (std::is_same_v<T, toml::value<toml::date>>) {
            return py::module_::import("datetime")
                .attr("date")
                .attr("fromisoformat")(time_string(element.get()));
        } else if constexpr (std::is_same_v<T, toml::value<toml::time>>) {
            return py::module_::import("datetime")
                .attr("time")
                .attr("fromisoformat")(time_string(element.get()));
        } else {
            return py::cast(element.get());
        }
    });
}

struct TomlValue final : FormatValue {
    TomlValue(TomlPtr root, Path scope)
        : root(std::move(root)),
          scope(std::move(scope)) {}

    Kind
    kind() const noexcept override {
        return Kind::toml;
    }

    std::string
    serialize() const override {
        if (scope.empty())
            return root->source;
        const auto& contents = table();
        if (auto preserved = view_source(*root, scope, contents))
            return std::move(*preserved);
        return format_table(contents);
    }

    py::object
    to_python() const override {
        return toml_to_python(table());
    }

    const toml::table&
    table() const {
        return selected_table(*root, scope);
    }

    TomlPtr root;
    Path scope;
};

struct TomlFragment final : FormatValue {
    explicit TomlFragment(toml::array node)
        : box(std::move(node)) {}

    Kind
    kind() const noexcept override {
        return Kind::toml;
    }

    std::string
    serialize() const override {
        if (const auto* table = box[0].as_table())
            return format_table(*table);
        return format_value(box[0]);
    }

    py::object
    to_python() const override {
        return toml_to_python(box[0]);
    }

    toml::array box;
};

Value
make_toml(TomlPtr root, Path scope = {}) {
    return std::make_shared<const TomlValue>(std::move(root), std::move(scope));
}

Value
make_fragment(const toml::node& node) {
    if (const auto* text = node.as_string())
        return Utf8(text->get());
    toml::array box;
    box.push_back(node);
    return std::make_shared<const TomlFragment>(std::move(box));
}

const TomlValue&
toml_value(const Value& value) {
    if (const auto* formatted = std::get_if<std::shared_ptr<const FormatValue>>(&value))
        if (const auto* native = dynamic_cast<const TomlValue*>(formatted->get()))
            return *native;
    throw Error(ErrorCode::type, "expected a TOML table");
}

std::string
strict_string(std::string_view bytes) {
    if (!utf8_characters(bytes))
        throw Error(ErrorCode::encoding, "TOML strings require valid UTF-8");
    return std::string(bytes);
}

std::string
strict_string(py::handle value) {
    const auto converted = from_python(value);
    if (const auto* bytes = std::get_if<Bytes>(&converted))
        return strict_string(std::string_view(bytes->data()));
    return std::get<Utf8>(converted).data();
}

void add_python(toml::array& output, py::handle value, std::unordered_set<PyObject*>& stack);

toml::table
python_table(py::handle value, std::unordered_set<PyObject*>& stack) {
    toml::table table;
    for (const auto& [key, child] : py::reinterpret_borrow<py::dict>(value)) {
        if (!py::isinstance<py::str>(key))
            throw Error(ErrorCode::type, "TOML table keys must be str");
        toml::array box;
        add_python(box, child, stack);
        table.insert(strict_string(key), box[0]);
    }
    return table;
}

bool
add_temporal(toml::array& output, py::handle value) {
    const auto module = py::module_::import("datetime");
    const bool is_time = py::isinstance(value, module.attr("time"));
    if (!is_time && !py::isinstance(value, module.attr("date")))
        return false;
    if (is_time && !value.attr("utcoffset")().is_none())
        throw Error(ErrorCode::type, "TOML local times cannot include a timezone");
    try {
        const auto literal = py::cast<std::string>(value.attr("isoformat")());
        const auto parsed = toml::parse("value = " + literal);
        const auto* element = parsed.get("value");
        if (parsed.size() == 1
            && element
            && (element->is_date() || element->is_time() || element->is_date_time())) {
            output.push_back(*element);
            return true;
        }
    } catch (const toml::parse_error&) {}
    throw Error(ErrorCode::type, "date/time value cannot be represented in TOML");
}

void
add_python(toml::array& output, py::handle value, std::unordered_set<PyObject*>& stack) {
    if (py::isinstance<py::str>(value) || py::isinstance<py::bytes>(value)) {
        output.push_back(strict_string(value));
    } else if (py::isinstance<py::bool_>(value)) {
        output.push_back(py::cast<bool>(value));
    } else if (py::isinstance<py::int_>(value)) {
        int overflow = 0;
        const auto number = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
        if (PyErr_Occurred())
            throw py::error_already_set();
        if (overflow)
            throw Error(ErrorCode::type, "TOML integers must fit in signed 64 bits");
        output.push_back(static_cast<std::int64_t>(number));
    } else if (py::isinstance<py::float_>(value)) {
        output.push_back(py::cast<double>(value));
    } else if (
        py::isinstance<py::dict>(value)
        || py::isinstance<py::list>(value)
        || py::isinstance<py::tuple>(value)
    ) {
        if (stack.size() >= 1024 || !stack.insert(value.ptr()).second)
            throw Error(ErrorCode::type, "cyclic or excessively nested TOML value");
        if (py::isinstance<py::dict>(value)) {
            output.push_back(python_table(value, stack));
        } else {
            toml::array array;
            for (auto item : py::reinterpret_borrow<py::sequence>(value))
                add_python(array, item, stack);
            output.push_back(std::move(array));
        }
        stack.erase(value.ptr());
    } else if (!add_temporal(output, value)) {
        throw Error(ErrorCode::type, "expected a TOML-compatible value (TOML has no null)");
    }
}

toml::array
box_value(const Value& value) {
    toml::array result;
    if (const auto* text = std::get_if<Utf8>(&value)) {
        result.push_back(text->data());
    } else if (const auto* bytes = std::get_if<Bytes>(&value)) {
        result.push_back(strict_string(std::string_view(bytes->data())));
    } else if (const auto* formatted = std::get_if<std::shared_ptr<const FormatValue>>(&value)) {
        if (const auto* table = dynamic_cast<const TomlValue*>(formatted->get()))
            result.push_back(table->table());
        else if (const auto* fragment = dynamic_cast<const TomlFragment*>(formatted->get()))
            result.push_back(fragment->box[0]);
        else
            throw Error(ErrorCode::type, "expected TOML value; convert the source explicitly");
    } else if (const auto* object = std::get_if<py::object>(&value)) {
        py::gil_scoped_acquire gil;
        std::unordered_set<PyObject*> stack;
        add_python(result, *object, stack);
    } else {
        throw Error(ErrorCode::type, "TOML values cannot be None");
    }
    return result;
}

JsonTree
json_from_toml(const toml::node& node) {
    return node.visit([](const auto& element) -> JsonTree {
        using T = std::remove_cvref_t<decltype(element)>;
        if constexpr (std::is_same_v<T, toml::table>) {
            JsonTree json(jsoncons::json_object_arg);
            for (const auto& [key, value] : element)
                json.try_emplace(key.str(), json_from_toml(value));
            return json;
        } else if constexpr (std::is_same_v<T, toml::array>) {
            JsonTree json(jsoncons::json_array_arg);
            for (const auto& value : element)
                json.push_back(json_from_toml(value));
            return json;
        } else if constexpr (std::is_same_v<T, toml::value<double>>) {
            if (!std::isfinite(element.get()))
                throw Error(
                    ErrorCode::serialization,
                    "JSON cannot represent TOML infinities or NaNs"
                );
            return element.get();
        } else if constexpr (
            std::is_same_v<T, toml::value<toml::date_time>>
            || std::is_same_v<T, toml::value<toml::date>>
            || std::is_same_v<T, toml::value<toml::time>>
        ) {
            return time_string(element.get());
        } else {
            return element.get();
        }
    });
}

void add_json(toml::array& box, const JsonTree& json);

toml::table
table_from_json(const JsonTree& json) {
    if (!json.is_object())
        throw Error(ErrorCode::type, "a TOML document must be a table");
    toml::table output;
    for (const auto& item : json.object_range()) {
        toml::array box;
        add_json(box, item.value());
        output.insert(item.key(), box[0]);
    }
    return output;
}

void
add_json(toml::array& box, const JsonTree& json) {
    if (json.is_object()) {
        box.push_back(table_from_json(json));
    } else if (json.is_array()) {
        toml::array out;
        for (const auto& item : json.array_range())
            add_json(out, item);
        box.push_back(std::move(out));
    } else if (json_string(json)) {
        box.push_back(json.as_string());
    } else if (json.is_bool()) {
        box.push_back(json.as<bool>());
    } else if (json.is_int64()) {
        box.push_back(json.as<std::int64_t>());
    } else if (json.is_double() && std::isfinite(json.as<double>())) {
        box.push_back(json.as<double>());
    } else {
        throw Error(ErrorCode::type, "JSON value cannot be represented in TOML");
    }
}

void
merge_table(toml::table& left, const toml::table& right, std::string_view conflict) {
    for (const auto& [key, incoming] : right) {
        auto* existing = left.get(key.str());
        if (!existing) {
            left.insert(key.str(), incoming);
        } else if (auto* table = existing->as_table(); table && incoming.is_table()) {
            merge_table(*table, *incoming.as_table(), conflict);
        } else if (conflict == "right") {
            left.insert_or_assign(key.str(), incoming);
        } else if (conflict == "error" && !equal(*existing, incoming)) {
            throw Error(
                ErrorCode::operation,
                "conflicting TOML merge key: " + std::string(key.str())
            );
        }
    }
}

Path
operand_path(Session& s, py::handle key) {
    const auto value = s.resolve(key);
    if (const auto* text = std::get_if<Utf8>(&value))
        return parse_path(text->data());
    throw Error(ErrorCode::type, "TOML key path must be a string");
}

void
append_dependencies(std::vector<NodePtr>& refs, py::handle value) {
    auto additional = dependencies(value);
    refs.insert(refs.end(), additional.begin(), additional.end());
}

Value
rename_value(const TomlValue& input, const Path& from, const Path& to) {
    auto updated = input.root->tree;
    auto old_parent_path = from;
    auto new_parent_path = to;
    old_parent_path.pop_back();
    new_parent_path.pop_back();
    auto* old_parent = ensure_table(updated, old_parent_path, false);
    if (!old_parent || !old_parent->contains(from.back()))
        throw Error(ErrorCode::operation, "TOML key does not exist: " + format_path(from));
    if (from == to)
        return make_toml(input.root, input.scope);
    const auto* original = lookup(input.root->tree, from);
    old_parent->erase(from.back());
    auto* new_parent = ensure_table(updated, new_parent_path, true);
    if (!new_parent->insert(to.back(), *original).second)
        throw Error(ErrorCode::operation, "TOML target key exists: " + format_path(to));
    if (old_parent_path == new_parent_path) {
        const auto& old_table = selected_table(*input.root, old_parent_path);
        const auto entry = old_table.find(from.back());
        const auto& key = entry->first;
        const Layout layout(input.root->source, input.root->tree);
        const auto begin = layout.offset(key.source().begin);
        const auto end = layout.offset(key.source().end);
        const auto replacement = format_key(to.back());
        std::vector<Edit> edits{{begin, end, replacement}};
        for (const auto& header : layout.headers()) {
            if (!has_prefix(header.path, from))
                continue;
            auto keys = layout.header_keys(header);
            if (keys.size() < from.size())
                continue;
            auto token = std::move(keys[from.size() - 1]);
            if (token.start == begin && token.end == end)
                continue;
            token.text = replacement;
            edits.push_back(std::move(token));
        }
        if (auto preserved = try_edits(layout, updated, std::move(edits)))
            return make_toml(std::move(preserved), input.scope);
    }
    return make_toml(reconcile(input.root, updated), input.scope);
}

} // namespace

Value
toml_from_json(const JsonTree& json) {
    return make_toml(canonical_source(table_from_json(json)));
}

void
bind_toml(py::module_& module) {
    register_format(
        Kind::toml,
        [](std::string_view input) {
            py::gil_scoped_release release;
            return make_toml(read_source(input));
        },
        [](py::handle value) {
            if (!py::isinstance<py::dict>(value))
                throw Error(ErrorCode::type, "a TOML document must be a table");
            std::unordered_set<PyObject*> stack{value.ptr()};
            return make_toml(canonical_source(python_table(value, stack)));
        }
    );
    py::class_<TOML, Source>(module, "TOML")
        .def(
            py::init([](py::str input) {
                return TOML(make_node(
                    "toml.parse",
                    Kind::toml,
                    {},
                    [input = std::move(input)](Session& s) {
                        const auto value = s.resolve(input);
                        py::gil_scoped_release release;
                        return make_toml(read_source(std::get<Utf8>(value).data()));
                    },
                    true,
                    {},
                    {},
                    "parse.toml"
                ));
            }),
            py::arg("value")
        )
        .def_static(
            "from_value",
            [](py::object value) {
                value = snapshot(value);
                auto refs = dependencies(value);
                return TOML(make_node(
                    "toml.from_value",
                    Kind::toml,
                    std::move(refs),
                    [value = std::move(value)](Session& s) {
                        const auto box = box_value(s.resolve(value));
                        const auto* table = box[0].as_table();
                        if (!table)
                            throw Error(ErrorCode::type, "a TOML document must be a table");
                        return make_toml(canonical_source(*table));
                    }
                ));
            },
            py::arg("value")
        )
        .def(
            "table",
            [](const TOML& self, py::object path) {
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, path);
                return TOML(make_node(
                    "toml.table",
                    Kind::toml,
                    std::move(refs),
                    [node = self.node, path = std::move(path)](Session& s) {
                        const auto input = s.eval(node);
                        const auto& value = toml_value(input);
                        auto scope = join_path(value.scope, operand_path(s, path));
                        selected_table(*value.root, scope);
                        return make_toml(value.root, std::move(scope));
                    }
                ));
            },
            py::arg("key")
        )
        .def(
            "root",
            [](const TOML& self) {
                return TOML(
                    make_node("toml.root", Kind::toml, {self.node}, [node = self.node](Session& s) {
                        return make_toml(toml_value(s.eval(node)).root);
                    })
                );
            }
        )
        .def(
            "get",
            [](const TOML& self, py::object key, py::args defaults) {
                if (defaults.size() > 1)
                    throw py::type_error("get accepts at most one default");
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, key);
                auto fallback = defaults.empty() ? py::object{} : snapshot(defaults[0]);
                if (fallback)
                    append_dependencies(refs, fallback);
                return Source(make_node(
                    "toml.get",
                    Kind::any,
                    std::move(refs),
                    [node = self.node,
                     key = std::move(key),
                     fallback = std::move(fallback)](Session& s) {
                        const auto input = s.eval(node);
                        const auto& value = toml_value(input);
                        const auto path = operand_path(s, key);
                        if (const auto* element = lookup(value.table(), path)) {
                            if (element->is_table())
                                return make_toml(value.root, join_path(value.scope, path));
                            return make_fragment(*element);
                        }
                        if (fallback)
                            return s.resolve(fallback);
                        throw Error(
                            ErrorCode::operation,
                            "TOML key does not exist: " + format_path(path)
                        );
                    }
                ));
            },
            py::arg("key")
        )
        .def(
            "set",
            [](const TOML& self, py::object key, py::object value) {
                value = snapshot(value);
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, key);
                append_dependencies(refs, value);
                return TOML(make_node(
                    "toml.set",
                    Kind::toml,
                    std::move(refs),
                    [node = self.node, key = std::move(key), value = std::move(value)](Session& s) {
                        const auto input = s.eval(node);
                        const auto& tree = toml_value(input);
                        auto path = join_path(tree.scope, operand_path(s, key));
                        const auto name = std::move(path.back());
                        path.pop_back();
                        auto updated = tree.root->tree;
                        const auto data = box_value(s.resolve(value));
                        ensure_table(updated, path, true)->insert_or_assign(name, data[0]);
                        return make_toml(reconcile(tree.root, updated), tree.scope);
                    }
                ));
            },
            py::arg("key"),
            py::arg("value")
        )
        .def(
            "remove",
            [](const TOML& self, py::object key) {
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, key);
                return TOML(make_node(
                    "toml.remove",
                    Kind::toml,
                    std::move(refs),
                    [node = self.node, key = std::move(key)](Session& s) {
                        const auto input = s.eval(node);
                        const auto& tree = toml_value(input);
                        auto path = join_path(tree.scope, operand_path(s, key));
                        const auto name = std::move(path.back());
                        path.pop_back();
                        auto updated = tree.root->tree;
                        if (auto* table = ensure_table(updated, path, false))
                            table->erase(name);
                        return make_toml(reconcile(tree.root, updated), tree.scope);
                    }
                ));
            },
            py::arg("key")
        )
        .def(
            "rename",
            [](const TOML& self, py::object key, py::object new_key) {
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, key);
                append_dependencies(refs, new_key);
                return TOML(make_node(
                    "toml.rename",
                    Kind::toml,
                    std::move(refs),
                    [node = self.node,
                     key = std::move(key),
                     new_key = std::move(new_key)](Session& s) {
                        const auto input = s.eval(node);
                        const auto& tree = toml_value(input);
                        const auto old_path = join_path(tree.scope, operand_path(s, key));
                        const auto new_path = join_path(tree.scope, operand_path(s, new_key));
                        return rename_value(tree, old_path, new_path);
                    }
                ));
            },
            py::arg("key"),
            py::arg("new_key")
        )
        .def(
            "merge",
            [](const TOML& self, py::object other, std::string conflict) {
                other = snapshot(other);
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, other);
                return TOML(make_node(
                    "toml.merge",
                    Kind::toml,
                    std::move(refs),
                    [node = self.node,
                     other = std::move(other),
                     conflict = std::move(conflict)](Session& s) {
                        if (conflict != "left" && conflict != "right" && conflict != "error")
                            throw Error(
                                ErrorCode::operation,
                                "TOML conflict policy must be left, right or error"
                            );
                        const auto input = s.eval(node);
                        const auto& tree = toml_value(input);
                        const auto incoming = box_value(s.resolve(other));
                        const auto* table = incoming[0].as_table();
                        if (!table)
                            throw Error(ErrorCode::type, "TOML merge requires an incoming table");
                        auto updated = tree.root->tree;
                        merge_table(*ensure_table(updated, tree.scope, false), *table, conflict);
                        return make_toml(reconcile(tree.root, updated), tree.scope);
                    },
                    true,
                    {},
                    {},
                    "merge"
                ));
            },
            py::arg("other"),
            py::kw_only(),
            py::arg("conflict") = "right"
        )
        .def(
            "keys",
            [](const TOML& self) {
                return Source(make_node(
                    "toml.keys",
                    Kind::any,
                    {self.node},
                    [node = self.node](Session& s) -> Value {
                        const auto input = s.eval(node);
                        const auto& table = toml_value(input).table();
                        std::vector<std::pair<toml::source_position, std::string_view>> order;
                        order.reserve(table.size());
                        for (const auto& [key, ignored] : table)
                            order.emplace_back(key.source().begin, key.str());
                        std::ranges::sort(order);
                        JsonTree keys(jsoncons::json_array_arg);
                        for (const auto& [_, key] : order)
                            keys.push_back(JsonTree(key));
                        return make_json(std::move(keys));
                    }
                ));
            }
        )
        .def(
            "arrays",
            [](const TOML& self, py::object key) {
                auto refs = std::vector<NodePtr>{self.node};
                append_dependencies(refs, key);
                const bool has_key = !key.is_none();
                return Source(make_node(
                    "toml.arrays",
                    Kind::any,
                    std::move(refs),
                    [node = self.node, key = std::move(key), has_key](Session& s) {
                        const auto input = s.eval(node);
                        const auto& tree = toml_value(input);
                        if (has_key) {
                            const auto path = operand_path(s, key);
                            const auto* item = lookup(tree.table(), path);
                            if (!item || !item->is_array())
                                throw Error(
                                    ErrorCode::type,
                                    "TOML path is not an array: " + format_path(path)
                                );
                            return make_fragment(*item);
                        }
                        toml::array arrays;
                        for (const auto& [_, item] : tree.table())
                            if (item.is_array())
                                arrays.push_back(item);
                        return make_fragment(arrays);
                    }
                ));
            },
            py::arg("key") = py::none()
        )
        .def("to_json", [](const TOML& self) {
            return JSON(
                make_node("toml.to_json", Kind::json, {self.node}, [node = self.node](Session& s) {
                    const auto value = s.eval(node);
                    return make_json(json_from_toml(toml_value(value).table()));
                })
            );
        });
}

} // namespace syntwine
