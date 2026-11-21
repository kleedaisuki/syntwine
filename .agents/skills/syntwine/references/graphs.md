# Lazy graphs and Python callbacks

Keep inputs as sources until the final build. Callbacks receive evaluated
values, not source objects. Choose the operation by what the callback returns:

| Use | Operation |
| --- | --- |
| Transform an evaluated value into another value | `.map(fn)` |
| Select a new source from an evaluated value | `.bind(fn)` |
| Select a value or source from build context | `Source.defer(fn)` with `.generate(context=...)` |
| Reuse a typed operation with an explicit input and output kind | `@operation(...)`, then `.pipe(op)` |

Return the selected source from `.bind()` or `Source.defer()`; do not call
`.generate()` inside the callback. The evaluator then resolves it in the
same session. Repeated pure nodes may be cached in that session. Mark callbacks
with external state or side effects `pure=False` where supported.

```python
from pathlib import Path

from syntwine import Document, MD, Source

# Given manifest.json containing {"entry": "body.md"} and sections/body.md:
def read_section(name: str) -> MD:
    return Source(Path("sections") / name).as_md()

section = Source("manifest.json").as_json().get_text("entry").bind(read_section)
doc = Document(format="md").append(MD("# Handbook"), section)
generated = doc.generate()
```

Relative paths resolve from the build process's working directory. When the
project uses another base directory, pass paths resolved against that directory.

```python
from syntwine import Text, operation

@operation(input=Text, output=Text, pure=True)
def strip_line_ends(text: str) -> str:
    return "\n".join(line.rstrip() for line in text.splitlines())

assert Text("First  \nSecond").pipe(strip_line_ends).generate().text == "First\nSecond"
```

`Document.pipe()` receives the joined document as bytes. To transform one
text or Markdown part, attach the operation to that part before appending it.
