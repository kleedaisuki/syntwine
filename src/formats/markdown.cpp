#include "syntwine/detail/formats/markdown.hpp"
#include "syntwine/detail/platform/utf8.hpp"
#include "syntwine/detail/telemetry/telemetry.hpp"

#include <cmark.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace syntwine {
namespace {

using Tree = std::unique_ptr<cmark_node, decltype(&cmark_node_free)>;

Tree
parse_tree(std::string_view source) {
    Tree result(
        cmark_parse_document(source.data(), source.size(), CMARK_OPT_DEFAULT),
        cmark_node_free
    );
    if (!result)
        throw Error(ErrorCode::parse, "could not parse CommonMark");
    return result;
}

class MarkdownValue final : public FormatValue, public std::enable_shared_from_this<MarkdownValue> {
public:
    explicit MarkdownValue(std::string source)
        : source_(std::move(source)) {}

    Kind
    kind() const noexcept override {
        return Kind::md;
    }

    std::string
    serialize() const override {
        return source_;
    }

    py::object to_python() const override;

    const std::string&
    source() const noexcept {
        return source_;
    }

    cmark_node*
    tree(Session* session = nullptr) const {
        std::call_once(once_, [&] {
            auto* trace = session ? session->trace() : nullptr;
            auto* node = trace ? session->active() : nullptr;
            telemetry::Span span(
                trace,
                "parse.md",
                telemetry::Level::operations,
                node ? node->id : 0,
                node ? std::string_view(node->source_id) : "",
                node ? std::string_view(node->name) : ""
            );
            if (trace)
                span.input_bytes(source_.size());
            tree_ = parse_tree(source_);
        });
        return tree_.get();
    }

private:
    std::string source_;
    mutable std::once_flag once_;
    mutable Tree tree_{nullptr, cmark_node_free};
};

using MarkdownPtr = std::shared_ptr<MarkdownValue>;

py::object
MarkdownValue::to_python() const {
    return py::cast(std::const_pointer_cast<MarkdownValue>(shared_from_this()));
}

const MarkdownValue&
markdown(const Value& value) {
    return static_cast<const MarkdownValue&>(*std::get<std::shared_ptr<const FormatValue>>(value));
}

Value
markdown_value(std::string source) {
    return std::make_shared<MarkdownValue>(std::move(source));
}

Value
parse_markdown(std::string_view source) {
    py::gil_scoped_release release;
    if (!utf8_characters(source))
        throw Error(ErrorCode::decode, "Markdown source must be UTF-8");
    return markdown_value(std::string(source));
}

Value
markdown_from_python(py::handle object) {
    if (py::isinstance<MarkdownValue>(object))
        return py::cast<MarkdownPtr>(object);
    if (py::isinstance<py::str>(object)) {
        auto text = from_python(object, Kind::text);
        return markdown_value(std::get<Utf8>(text).data());
    }
    throw Error(ErrorCode::type, "expected MarkdownValue or str");
}

bool
blank(std::string_view line) {
    return line.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

struct Lines {
    std::string_view source;
    std::vector<std::size_t> starts;

    explicit Lines(std::string_view source)
        : source(source),
          starts{0} {
        for (std::size_t p = 0; p < source.size(); ++p) {
            if (source[p] != '\r' && source[p] != '\n')
                continue;
            if (source[p] == '\r' && p + 1 < source.size() && source[p + 1] == '\n')
                ++p;
            starts.push_back(p + 1);
        }
    }

    std::size_t
    at(std::size_t i) const {
        return i < starts.size() ? starts[i] : source.size();
    }

    std::string_view
    line(std::size_t i) const {
        return source.substr(at(i), at(i + 1) - at(i));
    }
};

std::string_view
trim_inline(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

template <class Fn>
void
walk(cmark_node* root, Fn fn) {
    std::unique_ptr<cmark_iter, decltype(&cmark_iter_free)> iter(
        cmark_iter_new(root),
        cmark_iter_free
    );
    for (auto event = cmark_iter_next(iter.get()); event != CMARK_EVENT_DONE;
         event = cmark_iter_next(iter.get())) {
        if (event == CMARK_EVENT_ENTER)
            fn(cmark_iter_get_node(iter.get()));
    }
}

std::string
label(cmark_node* root) {
    std::string text;
    walk(root, [&](cmark_node* node) {
        switch (cmark_node_get_type(node)) {
        case CMARK_NODE_TEXT:
        case CMARK_NODE_CODE:
            text += cmark_node_get_literal(node);
            break;
        case CMARK_NODE_SOFTBREAK:
        case CMARK_NODE_LINEBREAK:
            text += ' ';
            break;
        default:
            break;
        }
    });
    return text;
}

struct Heading {
    cmark_node* node;
    std::string title;
    std::size_t first_line;
    std::size_t section_end;
    int level;
};

std::vector<Heading>
headings(const MarkdownValue& value) {
    std::vector<Heading> result;
    auto tree = value.tree();
    walk(tree, [&](cmark_node* node) {
        if (cmark_node_get_type(node) != CMARK_NODE_HEADING)
            return;
        auto parent = cmark_node_parent(node);
        const auto first = static_cast<std::size_t>(cmark_node_get_start_line(node) - 1);
        const auto end = parent != tree ? static_cast<std::size_t>(cmark_node_get_end_line(parent))
                                        : std::numeric_limits<std::size_t>::max();
        result.push_back({node, label(node), first, end, cmark_node_get_heading_level(node)});
    });

    // Each container has at most six open sections. A sibling's heading
    // closes higher or equal levels without scanning the rest of the source.
    std::unordered_map<cmark_node*, std::vector<Heading*>> open;
    for (auto& heading : result) {
        auto& stack = open[cmark_node_parent(heading.node)];
        while (!stack.empty() && stack.back()->level >= heading.level) {
            stack.back()->section_end = heading.first_line;
            stack.pop_back();
        }
        stack.push_back(&heading);
    }
    return result;
}

std::pair<std::size_t, std::size_t>
find_section(const MarkdownValue& value, std::string_view title, std::ptrdiff_t occurrence) {
    const auto entries = headings(value);
    std::vector<const Heading*> matches;
    for (const auto& heading : entries) {
        if (heading.title == title)
            matches.push_back(&heading);
    }
    if (occurrence < 0)
        occurrence += static_cast<std::ptrdiff_t>(matches.size());
    if (occurrence < 0 || static_cast<std::size_t>(occurrence) >= matches.size())
        throw Error(ErrorCode::operation, "Markdown section not found: " + std::string(title));
    auto& found = *matches[static_cast<std::size_t>(occurrence)];
    Lines lines(value.source());
    return {lines.at(found.first_line), lines.at(found.section_end)};
}

std::string_view
line_ending(std::string_view line) {
    if (line.ends_with("\r\n"))
        return "\r\n";
    if (line.ends_with("\n"))
        return "\n";
    if (line.ends_with("\r"))
        return "\r";
    return {};
}

std::size_t
column_offset(std::string_view line, int column, bool first_line) {
    // cmark reports byte offsets, excluding the optional initial UTF-8 BOM.
    const auto bom = first_line && line.starts_with("\xef\xbb\xbf") ? 3u : 0u;
    return std::min(line.size(), bom + static_cast<std::size_t>(column - 1));
}

std::size_t
quote_prefix(std::string_view line, std::size_t quotes) {
    std::size_t consumed = 0;
    while (quotes--) {
        const auto marker = line.find_first_not_of(" \t", consumed);
        if (marker == std::string_view::npos || line[marker] != '>')
            break;
        consumed = marker + 1;
    }
    return consumed;
}

struct Edit {
    std::size_t first;
    std::size_t last;
    std::string replacement;
};

std::optional<Edit>
shift_heading(const Heading& heading, const Lines& lines, int level) {
    auto start = heading.first_line;
    const auto first = lines.line(start);
    auto marker = column_offset(first, cmark_node_get_start_column(heading.node), start == 0);
    auto end_line = static_cast<std::size_t>(cmark_node_get_end_line(heading.node) - 1);
    if (start == end_line) {
        auto offset = lines.at(start) + marker;
        return Edit{
            offset,
            offset + static_cast<std::size_t>(heading.level),
            std::string(static_cast<std::size_t>(level), '#')
        };
    }

    // cmark can close a setext range on the following block's first line.
    if (auto next = cmark_node_next(heading.node))
        end_line =
            std::min(end_line, static_cast<std::size_t>(cmark_node_get_start_line(next) - 2));
    if (auto parent = cmark_node_parent(heading.node))
        end_line =
            std::min(end_line, static_cast<std::size_t>(cmark_node_get_end_line(parent) - 1));
    std::size_t quotes = 0;
    for (auto parent = cmark_node_parent(heading.node); parent; parent = cmark_node_parent(parent))
        quotes += cmark_node_get_type(parent) == CMARK_NODE_BLOCK_QUOTE;

    auto rule = heading.level == 1 ? '=' : '-';
    std::size_t mark = 0;
    std::size_t length = 0;
    for (; end_line > start; --end_line) {
        auto line = lines.line(end_line);
        auto content = line.substr(quote_prefix(line, quotes));
        auto candidate = trim_inline(content);
        if (!candidate.empty() && candidate.find_first_not_of(rule) == std::string_view::npos) {
            mark = static_cast<std::size_t>(candidate.data() - line.data());
            length = candidate.size();
            break;
        }
    }
    if (!length)
        return std::nullopt;
    if (level <= 2)
        return Edit{
            lines.at(end_line) + mark,
            lines.at(end_line) + mark + length,
            std::string(length, level == 1 ? '=' : '-')
        };

    // A reference definition can precede a setext title's first inline.
    // Relative inline lines survive cmark consuming those definitions.
    if (first.substr(marker).starts_with('[')) {
        int inline_end = 0;
        walk(heading.node, [&](cmark_node* node) {
            if (node != heading.node)
                inline_end = std::max(inline_end, cmark_node_get_end_line(node));
        });
        if (auto inline_lines = inline_end - static_cast<int>(start);
            inline_lines > 0 && static_cast<std::size_t>(inline_lines) < end_line - start) {
            start = end_line - static_cast<std::size_t>(inline_lines);
            auto line = lines.line(start);
            auto content = trim_inline(line.substr(quote_prefix(line, quotes)));
            if (!content.empty())
                marker = static_cast<std::size_t>(content.data() - line.data());
        }
    }

    // Setext has no syntax for H3–H6. Reuse the inline source, folding the
    // setext-only continuation lines into the single-line ATX construct.
    std::string replacement(static_cast<std::size_t>(level), '#');
    for (auto i = start; i < end_line; ++i) {
        const auto line = lines.line(i);
        auto skip = i == start ? marker : quote_prefix(line, quotes);
        replacement += ' ';
        replacement += trim_inline(line.substr(skip));
    }
    // A trailing hash run in setext is inline text, not a closing marker.
    auto end = replacement.find_last_not_of('#');
    if (end != std::string::npos
        && end + 1 < replacement.size()
        && (replacement[end] == ' ' || replacement[end] == '\t'))
        replacement.insert(end + 1, "\\");
    replacement += line_ending(lines.line(end_line));
    return Edit{lines.at(start) + marker, lines.at(end_line + 1), std::move(replacement)};
}

std::string
shift_headings(const MarkdownValue& value, int levels) {
    const auto& source = value.source();
    const auto entries = headings(value);
    Lines lines(source);
    std::string output;
    std::size_t from = 0;
    for (const auto& heading : entries) {
        auto level = std::clamp(heading.level + std::clamp(levels, -5, 5), 1, 6);
        if (level == heading.level)
            continue;
        auto edit = shift_heading(heading, lines, level);
        if (!edit)
            continue;
        output += lines.source.substr(from, edit->first - from);
        output += edit->replacement;
        from = edit->last;
    }
    if (!from)
        return source;
    output += lines.source.substr(from);
    return output;
}

void
append_block(std::string& left, std::string_view right) {
    if (left.empty() || right.empty()) {
        left += right;
        return;
    }

    const auto original_right = right;
    auto ending = line_ending(left);
    auto trailing = ending.empty() ? 0 : 1;
    if (!ending.empty()
        && !line_ending(std::string_view(left).substr(0, left.size() - ending.size())).empty())
        ++trailing;
    while (trailing < 2 && !right.empty()) {
        auto begin = right.starts_with("\r\n")
                         ? 2u
                         : (right.front() == '\r' || right.front() == '\n' ? 1u : 0u);
        if (begin == 0)
            break;
        ++trailing;
        if (ending.empty())
            ending = right.substr(0, begin);
        right = std::string_view(right.data() + begin, right.size() - begin);
        // Preserve the caller's bytes; only the inspection view moves.
    }
    auto missing = 2 - trailing;
    if (ending.empty() || ending == "\r")
        ending = ending == "\r" ? "\r\n" : "\n";
    if (left.back() == '\r' && original_right.front() == '\n')
        missing = std::max(1, missing + 1);
    for (auto i = 0; i < missing; ++i)
        left += ending;
    left += original_right;
}

template <class Fn>
NodePtr
md_node(const MD& source, std::string name, Kind type, Fn fn, bool structural = true) {
    auto root = source.node;
    return make_node(
        std::move(name),
        type,
        {root},
        [root, structural, fn = std::move(fn)](Session& session) {
            auto value = session.eval(root);
            const auto& md = markdown(value);
            if (structural) {
                py::gil_scoped_release release;
                (void)md.tree(&session);
            }
            return fn(md);
        }
    );
}

std::string
operand_text(Session& session, py::handle input) {
    auto value = session.resolve(input);
    if (auto text = std::get_if<Utf8>(&value))
        return text->data();
    return std::get<Utf8>(parse(Kind::text, serialize(value))).data();
}

MD
append_content(const MD& source, py::args fragments, bool prepend) {
    auto deps = dependencies(fragments);
    auto root = source.node;
    deps.insert(deps.begin(), root);
    return MD(make_node(
        prepend ? "md.prepend" : "md.append",
        Kind::md,
        std::move(deps),
        [root, fragments = py::tuple(fragments), prepend](Session& session) -> Value {
            auto base = session.eval(root);
            std::string out = prepend ? std::string{} : markdown(base).source();
            for (auto fragment : fragments)
                append_block(out, operand_text(session, fragment));
            if (prepend)
                append_block(out, markdown(base).source());
            return markdown_value(std::move(out));
        }
    ));
}

void
append_line(std::string& output, std::string_view source) {
    if (!output.empty()
        && !source.empty()
        && line_ending(output).empty()
        && source.front() != '\n'
        && source.front() != '\r')
        output += '\n';
    output += source;
}

enum class SectionEdit { before, after, replace, remove };

MD
edit_section(
    const MD& source,
    std::string title,
    py::object content,
    std::ptrdiff_t occurrence,
    SectionEdit edit,
    std::string name
) {
    auto deps = dependencies(content);
    auto root = source.node;
    deps.insert(deps.begin(), root);
    return MD(make_node(
        std::move(name),
        Kind::md,
        std::move(deps),
        [root, title = std::move(title), content = std::move(content), occurrence, edit](
            Session& session
        ) {
            auto base = session.eval(root);
            auto& value = markdown(base);
            auto [first, last] = [&] {
                py::gil_scoped_release release;
                (void)value.tree(&session);
                return find_section(value, title, occurrence);
            }();
            if (edit == SectionEdit::before)
                last = first;
            if (edit == SectionEdit::after)
                first = last;

            const std::string_view text = value.source();
            std::string output(text.substr(0, first));
            if (edit == SectionEdit::remove)
                output += text.substr(last);
            else {
                append_line(output, operand_text(session, content));
                append_line(output, text.substr(last));
            }
            return markdown_value(std::move(output));
        }
    ));
}

Value
heading_details(const MarkdownValue& value, bool sections) {
    const auto entries = [&] {
        py::gil_scoped_release release;
        return headings(value);
    }();
    py::list result;
    std::optional<Lines> lines;
    if (sections)
        lines.emplace(value.source());
    for (const auto& heading : entries) {
        py::dict entry;
        entry["title"] = heading.title;
        entry["level"] = heading.level;
        entry["line"] = heading.first_line + 1;
        if (sections) {
            const auto first = lines->at(heading.first_line);
            entry["source"] =
                py::str(lines->source.substr(first, lines->at(heading.section_end) - first));
        }
        result.append(std::move(entry));
    }
    return result;
}

Value
link_details(const MarkdownValue& value) {
    py::list result;
    walk(value.tree(), [&](cmark_node* node) {
        auto type = cmark_node_get_type(node);
        if (type != CMARK_NODE_LINK && type != CMARK_NODE_IMAGE)
            return;
        py::dict link;
        link["label"] = label(node);
        link["url"] = cmark_node_get_url(node);
        link["title"] = cmark_node_get_title(node);
        link["image"] = type == CMARK_NODE_IMAGE;
        result.append(std::move(link));
    });
    return result;
}

} // namespace

std::string
markdown_squish(std::string_view source) {
    if (!utf8_characters(source))
        throw Error(ErrorCode::decode, "Markdown source must be UTF-8");
    auto tree = parse_tree(source);
    Lines lines(source);
    std::string result;
    std::size_t emitted = 0;

    // Only gaps between independent top-level blocks are candidates. A
    // reference definition may have no AST node; leave any such gap alone.
    for (auto prev = cmark_node_first_child(tree.get()); prev && cmark_node_next(prev);
         prev = cmark_node_next(prev)) {
        const auto next = cmark_node_next(prev);
        const auto start = static_cast<std::size_t>(cmark_node_get_end_line(prev));
        const auto stop = static_cast<std::size_t>(cmark_node_get_start_line(next) - 1);
        if (stop <= start + 1
            || !blank(source.substr(lines.at(start), lines.at(stop) - lines.at(start))))
            continue;
        const auto keep = lines.at(start + 1);
        result += source.substr(emitted, keep - emitted);
        emitted = lines.at(stop);
    }
    if (emitted == 0)
        return std::string(source);
    result += source.substr(emitted);
    return result;
}

MD
markdown_codeblock(const Source& source, std::string language) {
    auto root = source.node;
    return MD(make_node(
        "md.codeblock",
        Kind::md,
        {root},
        [root, language = std::move(language)](Session& session) {
            if (language.find_first_of("\r\n") != std::string::npos)
                throw Error(ErrorCode::operation, "code block language must be on one line");
            auto contents = serialize(session.eval(root));
            py::gil_scoped_release release;
            if (!utf8_characters(contents))
                throw Error(ErrorCode::decode, "Markdown code block source must be UTF-8");
            auto fence_ch = language.find('`') == std::string::npos ? '`' : '~';
            std::size_t longest = 2;
            for (std::size_t i = 0, run = 0; i < contents.size(); ++i) {
                run = contents[i] == fence_ch ? run + 1 : 0;
                longest = std::max(longest, run);
            }
            std::string fence(longest + 1, fence_ch);
            if (!contents.empty() && line_ending(contents).empty())
                contents += '\n';
            auto separator = !language.empty() && language.front() == fence_ch ? " " : "";
            return markdown_value(fence + separator + language + '\n' + contents + fence + '\n');
        }
    ));
}

void
bind_markdown(py::module_& module) {
    py::class_<MarkdownValue, MarkdownPtr>(module, "MarkdownValue")
        .def_property_readonly("source", &MarkdownValue::source)
        .def("__str__", &MarkdownValue::source);
    register_format(Kind::md, parse_markdown, markdown_from_python);

    py::class_<MD, Source>(module, "MD")
        .def(
            py::init([](py::str text) {
                return MD(make_node(
                    "md.literal",
                    Kind::md,
                    {},
                    [text = py::object(std::move(text))](Session&) {
                        return markdown_from_python(text);
                    },
                    true,
                    {},
                    {},
                    "source.decode"
                ));
            }),
            py::arg("source")
        )
        .def(
            "section",
            [](const MD& self, std::string title, std::ptrdiff_t occurrence) {
                return MD(md_node(
                    self,
                    "md.section",
                    Kind::md,
                    [title = std::move(title), occurrence](const auto& value) {
                        py::gil_scoped_release release;
                        const auto [first, last] = find_section(value, title, occurrence);
                        return markdown_value(value.source().substr(first, last - first));
                    }
                ));
            },
            py::arg("heading"),
            py::arg("occurrence") = 0
        )
        .def(
            "sections",
            [](const MD& self) {
                return Source(md_node(self, "md.sections", Kind::any, [](const auto& value) {
                    return heading_details(value, true);
                }));
            }
        )
        .def(
            "headings",
            [](const MD& self) {
                return Source(md_node(self, "md.headings", Kind::any, [](const auto& value) {
                    return heading_details(value, false);
                }));
            }
        )
        .def(
            "shift_headings",
            [](const MD& self, int levels) {
                return MD(md_node(self, "md.shift_headings", Kind::md, [levels](const auto& value) {
                    py::gil_scoped_release release;
                    return markdown_value(shift_headings(value, levels));
                }));
            },
            py::arg("levels")
        )
        .def(
            "insert_before",
            [](const MD& self, std::string title, py::object content, std::ptrdiff_t occurrence) {
                return edit_section(
                    self,
                    std::move(title),
                    std::move(content),
                    occurrence,
                    SectionEdit::before,
                    "md.insert_before"
                );
            },
            py::arg("heading"),
            py::arg("content"),
            py::arg("occurrence") = 0
        )
        .def(
            "insert_after",
            [](const MD& self, std::string title, py::object content, std::ptrdiff_t occurrence) {
                return edit_section(
                    self,
                    std::move(title),
                    std::move(content),
                    occurrence,
                    SectionEdit::after,
                    "md.insert_after"
                );
            },
            py::arg("heading"),
            py::arg("content"),
            py::arg("occurrence") = 0
        )
        .def(
            "append",
            [](const MD& self, py::args content) {
                return append_content(self, std::move(content), false);
            }
        )
        .def(
            "prepend",
            [](const MD& self, py::args content) {
                return append_content(self, std::move(content), true);
            }
        )
        .def(
            "replace_section",
            [](const MD& self, std::string title, py::object content, std::ptrdiff_t occurrence) {
                return edit_section(
                    self,
                    std::move(title),
                    std::move(content),
                    occurrence,
                    SectionEdit::replace,
                    "md.replace_section"
                );
            },
            py::arg("heading"),
            py::arg("content"),
            py::arg("occurrence") = 0
        )
        .def(
            "remove_section",
            [](const MD& self, std::string title, std::ptrdiff_t occurrence) {
                return edit_section(
                    self,
                    std::move(title),
                    py::none(),
                    occurrence,
                    SectionEdit::remove,
                    "md.remove_section"
                );
            },
            py::arg("heading"),
            py::arg("occurrence") = 0
        )
        .def(
            "links",
            [](const MD& self) {
                return Source(md_node(self, "md.links", Kind::any, link_details));
            }
        )
        .def("render", [](const MD& self) {
            return Text(md_node(
                self,
                "md.render",
                Kind::text,
                [](const auto& value) -> Value { return Utf8{value.source()}; },
                false
            ));
        });
}

} // namespace syntwine
