# Work with document formats

`.as_*` serializes a source and interprets the resulting bytes in the named
format. To convert structured values between JSON and TOML, use
`JSON.to_toml()` or `TOML.to_json()` instead. Use `.from_value()` to start
from Python data; the plain `JSON(...)` and `TOML(...)` constructors take
serialized text.

## Markdown structure

Use heading and section operations instead of splitting on `#`. Use
`Document(format="md").squish()` for whole-document compaction that respects
code fences and Markdown blocks, not generic whitespace collapsing.

```python
from syntwine import MD

notes = MD("# Notes\n\n## Rules\n\n- Keep code blocks.\n\n## Appendix\n")
rules = notes.section("Rules").generate().text
assert "- Keep code blocks." in rules and "Appendix" not in rules
```

## JSON values and TOML documents

Use `JSON.get_text(key)` when a downstream operation needs a `str`, not the
serialized JSON string (with quotes). Use a TOML table view to make scoped
edits, then call `.root()` to retain the other tables in the result.

```python
from syntwine import Document, JSON, TOML, Text

message = JSON.from_value({"message": "Ready."}).get_text("message")
doc = Document().append(Text("Status"), message)
config = TOML('[agent]\nmode = "local"\n\n[ui]\ncolor = "blue"\n')
updated = config.table("agent").set("instructions", doc.as_literal()).root()
result = updated.generate().value
assert result["agent"]["instructions"] == "Status\n\nReady."
assert result["ui"]["color"] == "blue"
```

TOML decodes embedded bytes as strict UTF-8. Use `.base64()` or `.hex()` on
a literal if the input is arbitrary binary. When editing an existing file,
prefer `Source(path).as_toml()` and edits on that source over reconstructing
it from a dictionary if comments and formatting matter.

## XML content

Use structural operations such as `.xpath()`, `.set_attr()`, and
`.set_text()` when editing XML. Pass text as `Text` so it is escaped;
pass an `XML` source when the operand is markup.

```python
from syntwine import Text, XML

node = XML.element("summary", Text("x < y"), attrs={"status": "ready"})
assert node.generate().text == '<summary status="ready">x &lt; y</summary>'
```
