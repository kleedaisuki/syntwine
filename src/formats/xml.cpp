#include "syntwine/detail/formats/xml.hpp"
#include "syntwine/detail/platform/utf8.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/valid.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <pybind11/stl.h>
#include <ranges>
#include <unordered_set>

namespace syntwine {
namespace {

using RawDoc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

class Doc {
public:
    Doc() = default;

    explicit Doc(RawDoc doc)
        : state_(std::make_shared<State>(std::move(doc))) {}

    xmlDoc*
    get() const noexcept {
        return state_ ? state_->doc.get() : nullptr;
    }

    xmlDoc*
    operator->() const noexcept {
        return get();
    }

    explicit
    operator bool() const noexcept {
        return get() != nullptr;
    }

    std::unique_lock<std::mutex>
    lock() const {
        return std::unique_lock(state_->mutex);
    }

private:
    struct State {
        explicit State(RawDoc doc)
            : doc(std::move(doc)) {}

        RawDoc doc;
        std::mutex mutex;
    };

    std::shared_ptr<State> state_;
};

using Parser = std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)>;
using Context = std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)>;
using XPath = std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)>;
using XmlNode = std::unique_ptr<xmlNode, decltype(&xmlFreeNode)>;
using XmlBuffer = std::unique_ptr<xmlBuffer, decltype(&xmlBufferFree)>;

struct FreeString {
    void
    operator()(xmlChar* text) const noexcept {
        xmlFree(text);
    }
};

using XmlString = std::unique_ptr<xmlChar, FreeString>;

const xmlChar*
bytes(const char* text) {
    return reinterpret_cast<const xmlChar*>(text);
}

const xmlChar*
bytes(const std::string& text) {
    return bytes(text.c_str());
}

const char*
chars(const xmlChar* text) {
    return reinterpret_cast<const char*>(text);
}

struct NativeDocument {
    Doc doc;
    bool declaration = false;
};

struct XmlResult {
    NativeDocument tree;
    std::optional<std::vector<xmlNode*>> selection;
    std::optional<std::string> original;
};

class XMLValue final : public FormatValue, public std::enable_shared_from_this<XMLValue> {
public:
    explicit XMLValue(XmlResult result)
        : result(std::move(result)) {}

    Kind
    kind() const noexcept override {
        return Kind::xml;
    }

    std::string serialize() const override;

    py::object
    to_python() const override {
        return py::cast(std::const_pointer_cast<XMLValue>(shared_from_this()));
    }

    const XmlResult result;
};

using XmlPtr = std::shared_ptr<XMLValue>;

const XMLValue&
xml_value(const Value& value) {
    return static_cast<const XMLValue&>(*std::get<std::shared_ptr<const FormatValue>>(value));
}

Value
make_xml(XmlResult result) {
    return std::make_shared<XMLValue>(std::move(result));
}

Doc
new_doc() {
    auto* doc = xmlNewDoc(bytes("1.0"));
    if (!doc)
        throw std::bad_alloc();
    return Doc(RawDoc(doc, xmlFreeDoc));
}

int
size_int(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error(ErrorCode::limit, "XML input exceeds the native parser's size limit");
    return static_cast<int>(size);
}

constexpr int parser_flags = XML_PARSE_NONET
                             | XML_PARSE_NOERROR
                             | XML_PARSE_NOWARNING
                             | XML_PARSE_BIG_LINES
#if LIBXML_VERSION >= 21300
                             | XML_PARSE_NO_XXE
#endif
#if LIBXML_VERSION >= 21400
                             | XML_PARSE_NO_SYS_CATALOG
#endif
    ;

struct ParseAttempt {
    Doc doc;
    std::string message;
    std::string location;
};

ParseAttempt
read(std::string_view source, int column_adjustment = 0, bool expand_entities = true) {
    int flags = parser_flags;
#if LIBXML_VERSION >= 21300
    if (expand_entities)
        flags |= XML_PARSE_NOENT;
#endif
    Parser parser(xmlNewParserCtxt(), xmlFreeParserCtxt);
    if (!parser)
        throw std::bad_alloc();
    RawDoc doc(
        xmlCtxtReadMemory(
            parser.get(),
            source.data(),
            size_int(source.size()),
            nullptr,
            nullptr,
            flags
        ),
        xmlFreeDoc
    );
    const auto* info = xmlCtxtGetLastError(parser.get());
    if (doc && (!info || info->level < XML_ERR_ERROR))
        return {Doc(std::move(doc)), {}, {}};
    if (!info)
        return {{}, "could not parse XML", {}};
    auto message = info->message ? std::string(info->message) : std::string("could not parse XML");
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    const auto column = std::max(1, info->int2 - (info->line == 1 ? column_adjustment : 0));
    return {{}, std::move(message), std::to_string(info->line) + ":" + std::to_string(column)};
}

std::string_view
without_bom(std::string_view source) {
    return source.starts_with("\xef\xbb\xbf") ? source.substr(3) : source;
}

bool
initial_declaration(std::string_view source) {
    source = without_bom(source);
    return source.starts_with("<?xml ")
           || source.starts_with("<?xml\t")
           || source.starts_with("<?xml\n")
           || source.starts_with("<?xml\r");
}

std::vector<xmlNode*>
siblings(xmlNode* first) {
    std::vector<xmlNode*> result;
    for (auto node = first; node; node = node->next)
        result.push_back(node);
    return result;
}

std::vector<xmlNode*>
active_nodes(const XmlResult& result) {
    return result.selection ? *result.selection : siblings(result.tree.doc->children);
}

bool
external_declarations(const xmlDoc* doc) {
    auto* subset = doc->intSubset;
    if (!subset)
        return false;
    if (subset->ExternalID || subset->SystemID)
        return true;
    for (auto* decl = subset->children; decl; decl = decl->next) {
        if (decl->type != XML_ENTITY_DECL)
            continue;
        auto* entity = reinterpret_cast<xmlEntity*>(decl);
        if (entity->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY
            || entity->etype == XML_EXTERNAL_PARAMETER_ENTITY)
            return true;
    }
    return false;
}

XmlResult
parse_xml_result(std::string_view source, bool keep_original = true) {
    auto original = [&]() -> std::optional<std::string> {
        if (keep_original)
            return std::string(source);
        return std::nullopt;
    };
    if (source.empty())
        return {{new_doc(), false}, std::vector<xmlNode*>{}, std::string{}};
    auto full = read(source);
    // libxml's expansion mode drops references it cannot resolve without
    // external resources. Keep them rather than changing the document.
    if (full.doc && external_declarations(full.doc.get()))
        full = read(source, 0, false);
    if (full.doc) {
        // libxml's reader auto-detects BOM and XML-declared encodings.
        const bool declaration = initial_declaration(source)
                                 || full.doc->encoding != nullptr
                                 || full.doc->standalone != -1;
        return {{std::move(full.doc), declaration}, std::nullopt, original()};
    }

    if (initial_declaration(source) || !utf8_characters(source))
        throw Error(ErrorCode::parse, std::move(full.message), std::move(full.location));
    constexpr std::string_view prefix = "<_syntwine_fragment>";
    constexpr std::string_view suffix = "</_syntwine_fragment>";
    std::string framed;
    framed.reserve(prefix.size() + source.size() + suffix.size());
    framed += prefix;
    framed += without_bom(source);
    framed += suffix;
    auto fragment = read(framed, size_int(prefix.size()));
    if (!fragment.doc)
        throw Error(ErrorCode::parse, std::move(fragment.message), std::move(fragment.location));

    // Splice out the parse-only root. XPath must never see a synthetic node.
    auto root = xmlDocGetRootElement(fragment.doc.get());
    for (auto child : siblings(root->children)) {
        xmlUnlinkNode(child);
        if (!xmlAddPrevSibling(root, child)) {
            xmlFreeNode(child);
            throw std::bad_alloc();
        }
    }
    xmlUnlinkNode(root);
    xmlFreeNode(root);
    return {{std::move(fragment.doc), false}, std::nullopt, original()};
}

Value
parse_xml(std::string_view source) {
    py::gil_scoped_release release;
    return make_xml(parse_xml_result(source));
}

Value
xml_from_python(py::handle input) {
    if (py::isinstance<XMLValue>(input))
        return py::cast<XmlPtr>(input);
    if (py::isinstance<py::str>(input)) {
        auto text = from_python(input, Kind::text);
        return parse_xml(std::get<Utf8>(text).data());
    }
    if (py::isinstance<py::bytes>(input))
        return parse_xml(py::cast<std::string>(input));
    throw Error(ErrorCode::type, "expected XMLValue, bytes, or str");
}

void
check_entity_context(xmlNode* root, bool detached) {
    if (!external_declarations(root->doc))
        return;
    for (auto* node = root; node;) {
        if (node->type == XML_ENTITY_REF_NODE) {
            auto* decl = xmlGetDocEntity(node->doc, node->name);
            if (detached || !decl || decl->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)
                throw Error(
                    detached ? ErrorCode::serialization : ErrorCode::operation,
                    detached
                        ? "XML fragment depends on entity declarations in its original document"
                        : "XML text depends on an external entity"
                );
        } else if (node->children) {
            node = node->children;
            continue;
        }
        while (node != root && !node->next)
            node = node->parent;
        node = node == root ? nullptr : node->next;
    }
}

std::string
node_text(xmlNode* node) {
    check_entity_context(node, false);
    XmlString content(xmlNodeGetContent(node));
    return content ? chars(content.get()) : "";
}

xmlNode*
doc_node(xmlDoc* doc) {
    return reinterpret_cast<xmlNode*>(doc);
}

void
add_child(xmlNode* parent, XmlNode child, bool prepend = false) {
    auto* added = prepend && parent->children ? xmlAddPrevSibling(parent->children, child.get())
                                              : xmlAddChild(parent, child.get());
    if (!added)
        throw std::bad_alloc();
    child.release();
}

XmlNode
text_node(xmlDoc* doc, const std::string& content) {
    auto* node = xmlNewDocTextLen(doc, bytes(content), size_int(content.size()));
    if (!node)
        throw std::bad_alloc();
    return XmlNode(node, xmlFreeNode);
}

XmlNode
copy_node(xmlDoc* doc, xmlNode* node) {
    check_entity_context(node, true);
    if (node->type == XML_ATTRIBUTE_NODE || node->type == XML_ENTITY_REF_NODE)
        return text_node(doc, node_text(node));

    auto* copy = xmlDocCopyNode(node, doc, 1);
    if (!copy)
        throw std::bad_alloc();
    return XmlNode(copy, xmlFreeNode);
}

void
add_copy(xmlNode* parent, xmlNode* source, bool prepend = false) {
    auto node = copy_node(parent->doc, source);
    if (node->type == XML_ELEMENT_NODE && !node->ns && parent->type == XML_ELEMENT_NODE) {
        auto* inherited = xmlSearchNs(parent->doc, parent, nullptr);
        if (inherited && inherited->href && *inherited->href) {
            auto* declaration = node->nsDef;
            while (declaration && declaration->prefix)
                declaration = declaration->next;
            if (!declaration && !xmlNewNs(node.get(), bytes(""), nullptr))
                throw std::bad_alloc();
        }
    }
    add_child(parent, std::move(node), prepend);
}

bool
copyable(xmlNode* node) {
    return node->type != XML_DTD_NODE && node->type != XML_DOCUMENT_TYPE_NODE;
}

void
copy_active(xmlNode* parent, const XmlResult& result, bool prepend = false) {
    auto nodes = active_nodes(result);
    if (prepend)
        std::reverse(nodes.begin(), nodes.end());
    for (auto* node : nodes) {
        if (copyable(node))
            add_copy(parent, node, prepend);
    }
}

NativeDocument
clone_active(const XmlResult& result) {
    if (!result.selection) {
        auto* doc = xmlCopyDoc(result.tree.doc.get(), 1);
        if (!doc)
            throw std::bad_alloc();
        return {Doc(RawDoc(doc, xmlFreeDoc)), result.tree.declaration};
    }
    auto doc = new_doc();
    copy_active(doc_node(doc.get()), result);
    return {std::move(doc), false};
}

std::string
dump(const NativeDocument& tree) {
    XmlBuffer buf(xmlBufferCreate(), xmlBufferFree);
    if (!buf)
        throw std::bad_alloc();
    auto* doc = tree.doc.get();
    if (tree.declaration) {
        std::string declaration = "<?xml version=\"";
        declaration += doc->version ? chars(doc->version) : "1.0";
        declaration += "\" encoding=\"UTF-8\"";
        if (doc->standalone >= 0)
            declaration += doc->standalone == 1 ? " standalone=\"yes\"" : " standalone=\"no\"";
        declaration += "?>";
        if (xmlBufferAdd(buf.get(), bytes(declaration), size_int(declaration.size())) != 0)
            throw std::bad_alloc();
    }
    for (auto* child = doc->children; child; child = child->next) {
        if (xmlNodeDump(buf.get(), doc, child, 0, 0) < 0)
            throw Error(ErrorCode::serialization, "could not serialize XML node");
    }
    const auto size = xmlBufferLength(buf.get());
    if (!size)
        return {};
    return {chars(xmlBufferContent(buf.get())), static_cast<std::size_t>(size)};
}

std::string
XMLValue::serialize() const {
    if (result.original)
        return *result.original;
    const auto lock = result.tree.doc.lock();
    if (!result.selection)
        return dump(result.tree);
    return dump(clone_active(result));
}

Value
tree_value(NativeDocument tree) {
    return make_xml({std::move(tree), std::nullopt, std::nullopt});
}

Value
selection(const XmlResult& source, std::vector<xmlNode*> nodes) {
    return make_xml({source.tree, std::move(nodes), std::nullopt});
}

void
valid_text(const std::string& text) {
    if (!utf8_characters(text))
        throw Error(ErrorCode::encoding, "XML text must be UTF-8");
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto ch = static_cast<unsigned char>(text[i]);
        if ((ch < 0x20 && ch != '\r' && ch != '\n' && ch != '\t')
            || (ch == 0xef && std::string_view(text).substr(i).starts_with("\xef\xbf\xbe"))
            || (ch == 0xef && std::string_view(text).substr(i).starts_with("\xef\xbf\xbf")))
            throw Error(ErrorCode::serialization, "character is not permitted in XML 1.0");
    }
}

struct QualifiedName {
    std::string prefix, local;
};

QualifiedName
qualified_name(std::string name) {
    if (name.find('\0') != std::string::npos
        || !utf8_characters(name)
        || xmlValidateQName(bytes(name), 0) != 0)
        throw Error(ErrorCode::operation, "invalid XML qualified name: " + name);
    auto colon = name.find(':');
    if (colon == std::string::npos)
        return {{}, std::move(name)};
    return {name.substr(0, colon), name.substr(colon + 1)};
}

XmlNode
new_element(xmlDoc* doc, const QualifiedName& name) {
    auto* node = xmlNewDocNode(doc, nullptr, bytes(name.local), nullptr);
    if (!node)
        throw std::bad_alloc();
    return XmlNode(node, xmlFreeNode);
}

xmlNs*
find_namespace(xmlNode* node, const QualifiedName& name) {
    if (name.prefix.empty())
        return nullptr;
    auto* ns = xmlSearchNs(node->doc, node, bytes(name.prefix));
    if (!ns)
        throw Error(ErrorCode::operation, "XML namespace prefix is not declared: " + name.prefix);
    return ns;
}

bool
is_namespace_declaration(const QualifiedName& name) {
    return name.prefix == "xmlns" || (name.prefix.empty() && name.local == "xmlns");
}

void
set_attr(xmlNode* node, const QualifiedName& name, const std::string& value) {
    if (is_namespace_declaration(name)) {
        auto prefix_name = name.prefix.empty() ? std::string_view{} : std::string_view(name.local);
        if (prefix_name == "xmlns"
            || (!prefix_name.empty() && value.empty())
            || value == "http://www.w3.org/2000/xmlns/"
            || ((prefix_name == "xml") != (value == chars(XML_XML_NAMESPACE))))
            throw Error(ErrorCode::serialization, "invalid XML namespace declaration");
        const auto* prefix = prefix_name.empty() ? nullptr : bytes(name.local);
        if (!xmlNewNs(node, bytes(value), prefix))
            throw Error(ErrorCode::operation, "invalid or duplicate XML namespace declaration");
        return;
    }
    if (!xmlSetNsProp(node, find_namespace(node, name), bytes(name.local), bytes(value)))
        throw std::bad_alloc();
}

std::string
text_operand(const Value& value) {
    std::string output;
    if (auto* text = std::get_if<Utf8>(&value))
        output = text->data();
    else if (auto* raw = std::get_if<Bytes>(&value))
        output = raw->data();
    else if (
        auto* fmt = std::get_if<std::shared_ptr<const FormatValue>>(&value);
        fmt && (*fmt)->kind() == Kind::xml
    ) {
        const auto& result = xml_value(value).result;
        const auto lock = result.tree.doc.lock();
        for (auto* node : active_nodes(result)) {
            if (copyable(node))
                output += node_text(node);
        }
    } else {
        output = serialize(value);
    }
    valid_text(output);
    return output;
}

void
add_operand(xmlNode* parent, const Value& value, bool prepend = false) {
    auto* fmt = std::get_if<std::shared_ptr<const FormatValue>>(&value);
    if (fmt && (*fmt)->kind() == Kind::xml) {
        const auto& result = xml_value(value).result;
        const auto lock = result.tree.doc.lock();
        copy_active(parent, result, prepend);
    } else {
        auto text = text_operand(value);
        if (!text.empty())
            add_child(parent, text_node(parent->doc, text), prepend);
    }
}

std::vector<Value>
resolve_contents(Session& session, const py::tuple& contents) {
    std::vector<Value> values;
    values.reserve(contents.size());
    for (auto value : contents)
        values.push_back(session.resolve(value));
    return values;
}

XML
create_element(std::string name, py::args children, py::object attrs, py::kwargs kwargs) {
    py::dict merged;
    if (!attrs.is_none())
        merged.attr("update")(attrs);
    merged.attr("update")(kwargs);
    auto deps = dependencies(children);
    auto attr_deps = dependencies(merged);
    deps.insert(deps.end(), attr_deps.begin(), attr_deps.end());

    return XML(make_node(
        "xml.element",
        Kind::xml,
        std::move(deps),
        [name = std::move(name),
         children = py::tuple(children),
         attrs = std::move(merged)](Session& session) {
            auto qualified = qualified_name(name);
            auto doc = new_doc();
            auto node = new_element(doc.get(), qualified);
            // Namespace declarations are in scope regardless of attribute order.
            for (auto entry : attrs) {
                auto key = py::cast<std::string>(entry.first);
                if (key == "xmlns" || key.starts_with("xmlns:"))
                    set_attr(
                        node.get(),
                        qualified_name(key),
                        text_operand(session.resolve(entry.second))
                    );
            }
            if (!qualified.prefix.empty())
                xmlSetNs(node.get(), find_namespace(node.get(), qualified));
            else if (
                auto* ns = xmlSearchNs(doc.get(), node.get(), nullptr); ns && ns->href && *ns->href
            )
                xmlSetNs(node.get(), ns);
            for (auto entry : attrs) {
                auto key = py::cast<std::string>(entry.first);
                if (key != "xmlns" && !key.starts_with("xmlns:"))
                    set_attr(
                        node.get(),
                        qualified_name(key),
                        text_operand(session.resolve(entry.second))
                    );
            }
            for (auto child : children)
                add_operand(node.get(), session.resolve(child));
            xmlDocSetRootElement(doc.get(), node.release());
            return tree_value({std::move(doc), false});
        }
    ));
}

using Namespaces = std::vector<std::pair<std::string, std::string>>;

Namespaces
namespaces_from_python(py::handle mapping) {
    Namespaces values;
    if (!mapping.is_none()) {
        for (auto each : mapping.attr("items")()) {
            auto pair = py::cast<py::tuple>(each);
            values.emplace_back(py::cast<std::string>(pair[0]), py::cast<std::string>(pair[1]));
        }
    }
    return values;
}

void
xpath_namespace(Context& context, const xmlChar* prefix, const xmlChar* uri) {
    if (!prefix || !*prefix || !uri || !*uri || xmlXPathRegisterNs(context.get(), prefix, uri) != 0)
        throw Error(
            ErrorCode::operation,
            "XPath namespace mappings need a non-empty prefix and URI"
        );
}

void
xpath_error(void*, const xmlError*) {}

std::vector<xmlNode*>
xpath_nodes(const XmlResult& source, const std::string& expression, const Namespaces& namespaces) {
    auto* doc = source.tree.doc.get();
    Context context(xmlXPathNewContext(doc), xmlXPathFreeContext);
    if (!context)
        throw std::bad_alloc();
#if LIBXML_VERSION >= 21200
    context->error = xpath_error;
#endif
    if (expression.find('\0') != std::string::npos || expression.empty())
        throw Error(ErrorCode::operation, "invalid XPath expression");
    std::unique_ptr<xmlXPathCompExpr, decltype(&xmlXPathFreeCompExpr)> compiled(
        xmlXPathCtxtCompile(context.get(), bytes(expression)),
        xmlXPathFreeCompExpr
    );
    if (!compiled)
        throw Error(ErrorCode::operation, "invalid XPath expression: " + expression);

    std::vector<xmlNode*> nodes;
    std::unordered_set<xmlNode*> seen;
    auto roots = active_nodes(source);
    if (!source.selection) {
        std::erase_if(roots, [](auto* node) { return node->type != XML_ELEMENT_NODE; });
        if (roots.empty())
            roots.push_back(doc_node(doc));
    }
    for (auto* root : roots) {
        xmlXPathRegisteredNsCleanup(context.get());
        auto* scope = root->type == XML_ATTRIBUTE_NODE ? root->parent : root;
        for (auto* up = scope; up && up->type == XML_ELEMENT_NODE; up = up->parent) {
            for (auto* ns = up->nsDef; ns; ns = ns->next) {
                if (ns->prefix && !xmlXPathNsLookup(context.get(), ns->prefix))
                    xpath_namespace(context, ns->prefix, ns->href);
            }
        }
        for (const auto& [prefix, uri] : namespaces) {
            if (prefix.find('\0') != std::string::npos || uri.find('\0') != std::string::npos)
                throw Error(ErrorCode::operation, "invalid XPath namespace mapping");
            xpath_namespace(context, bytes(prefix), bytes(uri));
        }
        context->node = root;
        XPath matches(xmlXPathCompiledEval(compiled.get(), context.get()), xmlXPathFreeObject);
        if (!matches)
            throw Error(ErrorCode::operation, "XPath evaluation failed: " + expression);
        if (matches->type != XPATH_NODESET)
            throw Error(ErrorCode::type, "XML.xpath requires an XPath expression yielding nodes");
        auto set = matches->nodesetval;
        if (!set)
            continue;
        xmlXPathNodeSetSort(set);
        for (int i = 0; i < set->nodeNr; ++i) {
            auto* node = set->nodeTab[i];
            if (node->type == XML_NAMESPACE_DECL)
                throw Error(ErrorCode::type, "namespace declarations are not XML content nodes");
            if (node->type == XML_DOCUMENT_NODE) {
                for (auto* child = node->children; child; child = child->next) {
                    if (seen.insert(child).second)
                        nodes.push_back(child);
                }
            } else if (seen.insert(node).second) {
                nodes.push_back(node);
            }
        }
    }
    // The order from each individual context is already document order.
    // Merge overlapping context results into one ordered selection.
    if (roots.size() > 1)
        std::ranges::stable_sort(nodes, [](auto* a, auto* b) {
            return xmlXPathCmpNodes(a, b) == 1;
        });
    return nodes;
}

template <class Fn>
NodePtr
xml_node(const XML& source, std::string name, Kind kind, Fn fn) {
    auto root = source.node;
    return make_node(std::move(name), kind, {root}, [root, fn = std::move(fn)](Session& session) {
        auto value = session.eval(root);
        const auto& result = xml_value(value).result;
        py::gil_scoped_release release;
        const auto lock = result.tree.doc.lock();
        return fn(result);
    });
}

template <class Fn>
XML
modify(const XML& source, std::string name, py::object operand, Fn fn) {
    auto deps = dependencies(operand);
    auto root = source.node;
    deps.insert(deps.begin(), root);
    return XML(make_node(
        std::move(name),
        Kind::xml,
        std::move(deps),
        [root, operand = std::move(operand), fn = std::move(fn)](Session& session) {
            auto value = session.eval(root);
            auto tree = [&] {
                const auto& result = xml_value(value).result;
                py::gil_scoped_release release;
                const auto lock = result.tree.doc.lock();
                return clone_active(result);
            }();
            fn(tree, session, operand);
            return tree_value(std::move(tree));
        }
    ));
}

template <class Fn>
void
on_elements(const NativeDocument& tree, Fn fn) {
    for (auto* node = tree.doc->children; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE)
            fn(node);
    }
}

bool
name_matches(xmlNode* node, const std::string& name) {
    std::string_view local(chars(node->name));
    if (name.starts_with("{")) {
        auto close = name.find('}');
        return close != std::string::npos
               && local == std::string_view(name).substr(close + 1)
               && node->ns
               && node->ns->href
               && std::string_view(chars(node->ns->href))
                      == std::string_view(name).substr(1, close - 1);
    }
    auto colon = name.find(':');
    if (colon == std::string::npos)
        return local == name;
    return local == std::string_view(name).substr(colon + 1)
           && node->ns
           && node->ns->prefix
           && std::string_view(chars(node->ns->prefix)) == std::string_view(name).substr(0, colon);
}

XML
select_xpath(const XML& self, std::string expression, py::handle namespace_map, bool first) {
    auto namespaces = namespaces_from_python(namespace_map);
    return XML(xml_node(
        self,
        first ? "xml.find" : "xml.xpath",
        Kind::xml,
        [expression = std::move(expression),
         namespaces = std::move(namespaces),
         first](const XmlResult& input) {
            auto found = xpath_nodes(input, expression, namespaces);
            if (first && found.size() > 1)
                found.resize(1);
            return selection(input, std::move(found));
        }
    ));
}

void
clear_node(xmlNode* node) {
    xmlUnlinkNode(node);
    xmlFreeNode(node);
}

XML
remove_nodes(const XML& self, std::optional<std::string> path, py::handle namespace_map) {
    auto namespaces = namespaces_from_python(namespace_map);
    return XML(xml_node(
        self,
        "xml.remove",
        Kind::xml,
        [path = std::move(path), namespaces = std::move(namespaces)](const XmlResult& source) {
            if (!path)
                return tree_value({new_doc(), false});
            auto tree = clone_active(source);
            XmlResult view{tree, std::nullopt, std::nullopt};
            auto removed = xpath_nodes(view, *path, namespaces);
            std::unordered_set<xmlNode*> marked(removed.begin(), removed.end());
            std::erase_if(removed, [&](auto* node) {
                for (auto* parent = node->parent; parent; parent = parent->parent) {
                    if (marked.contains(parent))
                        return true;
                }
                return false;
            });
            for (auto* node : removed) {
                if (node->type == XML_ATTRIBUTE_NODE)
                    xmlRemoveProp(reinterpret_cast<xmlAttr*>(node));
                else if (copyable(node))
                    clear_node(node);
            }
            return tree_value(std::move(tree));
        }
    ));
}

void
replace_text(const NativeDocument& tree, const std::string& text) {
    auto* doc = tree.doc.get();
    for (auto* next = doc->children; next;) {
        auto* node = next;
        next = next->next;
        if (node->type == XML_ELEMENT_NODE) {
            xmlFreeNodeList(node->children);
            node->last = node->children = nullptr;
            if (!text.empty())
                add_child(node, text_node(doc, text));
        } else if (
            node->type == XML_TEXT_NODE
            || node->type == XML_CDATA_SECTION_NODE
            || node->type == XML_ENTITY_REF_NODE
        ) {
            auto replacement = text_node(doc, text);
            xmlReplaceNode(node, replacement.release());
            xmlFreeNode(node);
        }
    }
}

void
strip_element_whitespace(const NativeDocument& tree, bool heuristic) {
    struct Frame {
        xmlNode* node;
        bool preserve;
    };

    std::vector<Frame> pending;
    on_elements(tree, [&](auto* root) { pending.push_back({root, false}); });
    while (!pending.empty()) {
        auto [node, preserve] = pending.back();
        pending.pop_back();
        if (auto local = XmlString(xmlGetNsProp(node, bytes("space"), XML_XML_NAMESPACE))) {
            const std::string_view mode(chars(local.get()));
            if (mode == "default" || mode == "preserve")
                preserve = mode == "preserve";
        }

        auto* dtd = tree.doc->intSubset;
        auto* decl =
            dtd ? xmlGetDtdQElementDesc(dtd, node->name, node->ns ? node->ns->prefix : nullptr)
                : nullptr;
        bool strip = !preserve && decl && decl->etype == XML_ELEMENT_TYPE_ELEMENT;
        if (!preserve && heuristic && !strip) {
            bool elements = false;
            bool significant = false;
            for (auto* child = node->children; child; child = child->next) {
                elements = elements || child->type == XML_ELEMENT_NODE;
                significant = significant
                              || child->type == XML_CDATA_SECTION_NODE
                              || child->type == XML_ENTITY_REF_NODE
                              || (child->type == XML_TEXT_NODE && !xmlIsBlankNode(child));
            }
            strip = elements && !significant;
        }
        for (auto* next = node->children; next;) {
            auto* child = next;
            next = child->next;
            if (child->type == XML_ELEMENT_NODE)
                pending.push_back({child, preserve});
            else if (strip && child->type == XML_TEXT_NODE && xmlIsBlankNode(child))
                clear_node(child);
        }
    }
}

bool
heuristic_policy(std::string_view policy) {
    if (policy == "markup_only")
        return false;
    if (policy == "element_only")
        return true;
    throw Error(ErrorCode::operation, "unknown XML whitespace policy: " + std::string(policy));
}

Value
minify(const XmlResult& input, std::string_view policy) {
    if (policy == "preserve")
        return make_xml(input);
    const bool heuristic = heuristic_policy(policy);
    auto tree = clone_active(input);
    if (heuristic && input.selection) {
        auto selected = *input.selection;
        std::erase_if(selected, [](auto* node) { return node->type != XML_ELEMENT_NODE; });
        std::size_t index = 0;
        on_elements(tree, [&](auto* root) {
            auto* original = selected[index++];
            if (xmlNodeGetSpacePreserve(original) == 1
                && xmlNodeGetSpacePreserve(root) != 1
                && xmlNodeSetSpacePreserve(root, 1) != 0)
                throw std::bad_alloc();
        });
    }
    strip_element_whitespace(tree, heuristic);
    return tree_value(std::move(tree));
}

XML
literal_source(py::object source) {
    if (!py::isinstance<py::str>(source) && !py::isinstance<py::bytes>(source))
        throw py::type_error("XML expects str or bytes");
    return XML(make_node(
        "xml.literal",
        Kind::xml,
        {},
        [source = std::move(source)](Session&) { return xml_from_python(source); },
        true,
        {},
        {},
        "parse.xml"
    ));
}

py::bytes
xml_source_bytes(const XMLValue& value) {
    return py::bytes(value.serialize());
}

XML
xpath_source(const XML& self, std::string expression, py::object namespaces) {
    return select_xpath(self, std::move(expression), namespaces, false);
}

XML
find_source(const XML& self, std::string path, py::object namespaces) {
    return select_xpath(self, std::move(path), namespaces, true);
}

XML
children_source(const XML& self, std::optional<std::string> name) {
    return XML(xml_node(
        self,
        "xml.children",
        Kind::xml,
        [name = std::move(name)](const XmlResult& source) {
            std::vector<xmlNode*> out;
            std::unordered_set<xmlNode*> seen;
            for (auto* root : active_nodes(source)) {
                if (root->type != XML_ELEMENT_NODE)
                    continue;
                for (auto* child = root->children; child; child = child->next) {
                    if (child->type == XML_ELEMENT_NODE
                        && (!name || name_matches(child, *name))
                        && seen.insert(child).second)
                        out.push_back(child);
                }
            }
            return selection(source, std::move(out));
        }
    ));
}

Text
text_source(const XML& self, std::string separator) {
    return Text(xml_node(
        self,
        "xml.text",
        Kind::text,
        [separator = std::move(separator)](const XmlResult& source) -> Value {
            std::string out;
            bool first = true;
            for (auto* node : active_nodes(source)) {
                if (node->type == XML_COMMENT_NODE || node->type == XML_PI_NODE || !copyable(node))
                    continue;
                if (!first)
                    out += separator;
                out += node_text(node);
                first = false;
            }
            return Utf8{std::move(out)};
        }
    ));
}

XML
set_text_source(const XML& self, py::object value) {
    return modify(
        self,
        "xml.set_text",
        std::move(value),
        [](NativeDocument& tree, Session& session, py::handle arg) {
            auto text = text_operand(session.resolve(arg));
            py::gil_scoped_release release;
            replace_text(tree, text);
        }
    );
}

XML
set_attr_source(const XML& self, std::string name, py::object value) {
    return modify(
        self,
        "xml.set_attr",
        std::move(value),
        [name = std::move(name)](NativeDocument& tree, Session& session, py::handle arg) {
            auto attr = qualified_name(name);
            if (is_namespace_declaration(attr))
                throw Error(
                    ErrorCode::operation,
                    "declare XML namespaces in XML.element(attrs=...)"
                );
            auto text = text_operand(session.resolve(arg));
            py::gil_scoped_release release;
            on_elements(tree, [&](xmlNode* root) { set_attr(root, attr, text); });
        }
    );
}

XML
remove_attr_source(const XML& self, std::string name) {
    return modify(
        self,
        "xml.remove_attr",
        py::none(),
        [name = std::move(name)](NativeDocument& tree, Session&, py::handle) {
            auto attr = qualified_name(name);
            if (is_namespace_declaration(attr))
                throw Error(ErrorCode::operation, "XML namespace declarations are not attributes");
            py::gil_scoped_release release;
            on_elements(tree, [&](xmlNode* root) {
                auto* ns = find_namespace(root, attr);
                xmlUnsetNsProp(root, ns, bytes(attr.local));
            });
        }
    );
}

XML
append_source(const XML& self, py::args contents) {
    return modify(
        self,
        "xml.append",
        std::move(contents),
        [](NativeDocument& tree, Session& session, py::handle arg) {
            auto operands = resolve_contents(session, py::reinterpret_borrow<py::tuple>(arg));
            on_elements(tree, [&](xmlNode* root) {
                for (const auto& value : operands)
                    add_operand(root, value);
            });
        }
    );
}

XML
prepend_source(const XML& self, py::args contents) {
    return modify(
        self,
        "xml.prepend",
        std::move(contents),
        [](NativeDocument& tree, Session& session, py::handle arg) {
            auto operands = resolve_contents(session, py::reinterpret_borrow<py::tuple>(arg));
            on_elements(tree, [&](xmlNode* root) {
                for (const auto& value : std::views::reverse(operands))
                    add_operand(root, value, true);
            });
        }
    );
}

XML
replace_source(const XML& self, py::args contents) {
    return modify(
        self,
        "xml.replace",
        std::move(contents),
        [](NativeDocument& tree, Session& session, py::handle arg) {
            auto operands = resolve_contents(session, py::reinterpret_borrow<py::tuple>(arg));
            tree = {new_doc(), false};
            for (const auto& value : operands)
                add_operand(doc_node(tree.doc.get()), value);
        }
    );
}

XML
wrap_xml_source(const XML& self, std::string name) {
    return XML(
        xml_node(self, "xml.wrap", Kind::xml, [name = std::move(name)](const XmlResult& source) {
            auto doc = new_doc();
            auto qualified = qualified_name(name);
            auto root = new_element(doc.get(), qualified);
            if (!qualified.prefix.empty()) {
                auto nodes = active_nodes(source);
                xmlNs* ns = nullptr;
                for (auto* node : nodes) {
                    if (node->type == XML_ELEMENT_NODE)
                        ns = xmlSearchNs(node->doc, node, bytes(qualified.prefix));
                    if (ns)
                        break;
                }
                if (!ns)
                    throw Error(
                        ErrorCode::operation,
                        "XML wrapper namespace prefix is not declared: " + qualified.prefix
                    );
                auto* exported = xmlNewNs(root.get(), ns->href, ns->prefix);
                if (!exported)
                    throw std::bad_alloc();
                xmlSetNs(root.get(), exported);
            }
            copy_active(root.get(), source);
            xmlDocSetRootElement(doc.get(), root.release());
            return tree_value({std::move(doc), false});
        })
    );
}

XML
unwrap_source(const XML& self) {
    return XML(xml_node(self, "xml.unwrap", Kind::xml, [](const XmlResult& source) {
        auto doc = new_doc();
        auto* parent = doc_node(doc.get());
        for (auto* root : active_nodes(source)) {
            if (root->type == XML_ELEMENT_NODE) {
                for (auto* child = root->children; child; child = child->next)
                    add_copy(parent, child);
            } else if (copyable(root)) {
                add_copy(parent, root);
            }
        }
        return tree_value({std::move(doc), false});
    }));
}

XML
minify_source(const XML& self, std::string policy) {
    return XML(xml_node(
        self,
        "xml.minify_whitespace",
        Kind::xml,
        [policy = std::move(policy)](const XmlResult& input) { return minify(input, policy); }
    ));
}

Literal
serialize_source(const XML& self) {
    auto root = self.node;
    return Literal(
        make_node("xml.serialize", Kind::literal, {root}, [root](Session& session) -> Value {
            return Bytes{serialize(session.eval(root))};
        })
    );
}

} // namespace

std::string
xml_squish(std::string_view source, std::string_view policy) {
    auto parsed = parse_xml_result(source, false);
    if (policy == "preserve")
        return std::string(source);
    strip_element_whitespace(parsed.tree, heuristic_policy(policy));
    return dump(parsed.tree);
}

void
bind_xml(py::module_& module) {
    py::class_<XMLValue, XmlPtr> value(module, "XMLValue");
    value.def_property_readonly("source", &xml_source_bytes);
    register_format(Kind::xml, parse_xml, xml_from_python);

    py::class_<XML, Source> xml(module, "XML");
    xml.def(py::init(&literal_source), py::arg("source"));
    xml.def_static("element", &create_element, py::arg("name"), py::arg("attrs") = py::none());
    xml.def("xpath", &xpath_source, py::arg("expression"), py::arg("namespaces") = py::none());
    xml.def("find", &find_source, py::arg("path"), py::arg("namespaces") = py::none());
    xml.def("children", &children_source, py::arg("name") = py::none());
    xml.def("text", &text_source, py::arg("separator") = "");
    xml.def("set_text", &set_text_source, py::arg("value"));
    xml.def("set_attr", &set_attr_source, py::arg("name"), py::arg("value"));
    xml.def("remove_attr", &remove_attr_source, py::arg("name"));
    xml.def("append", &append_source);
    xml.def("prepend", &prepend_source);
    xml.def("replace", &replace_source);
    xml.def(
        "remove",
        &remove_nodes,
        py::arg("path") = py::none(),
        py::arg("namespaces") = py::none()
    );
    xml.def("wrap", &wrap_xml_source, py::arg("name"));
    xml.def("unwrap", &unwrap_source);
    xml.def("minify_whitespace", &minify_source, py::arg("policy") = "markup_only");
    xml.def("serialize", &serialize_source);
}

} // namespace syntwine
