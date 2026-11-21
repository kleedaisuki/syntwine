---
name: syntwine
description: Assemble, transform, or debug documents with the Syntwine Python API, including lazy sources, structured fragments, and output budgets. Use when a task uses or asks for Syntwine, not for general document editing or changes to its native engine.
---

# Use Syntwine

## Start here

Use the project's existing Python environment and check that it can import
`syntwine`. On Unix an existing `.venv` uses `.venv/bin/python`; on Windows it
uses `.venv\Scripts\python.exe`. If Syntwine is missing, select the
[release wheel](https://github.com/kleedaisuki/syntwine/releases) matching the
interpreter, OS, and architecture. The v0.1.0 wheels target CPython 3.15.
Install the downloaded file into that environment with
`uv pip install --python <env-python> <wheel-file>`, or use that interpreter's
`-m pip install <wheel-file>` if pip is available.

```python
from syntwine import Document, MD, Text

doc = Document(format="md").append(MD("# Notes"), Text("Ready."))
artifact = doc.generate()
assert artifact.text == "# Notes\n\nReady."
```

- For a file, use `Source(path)` and choose `.as_text()`, `.as_md()`, or a
  structured format. `Source("...")` treats a string as a path. For inline
  content, use `Text("...")`, `Literal(b"...")`, or a format constructor.
- Build the graph before calling `.generate()` for an in-memory result or
  `.write(path)` for an atomic file replacement. For documents, parts are
  joined before whole-document operations such as `.squish()` and `.replace()`.
  These methods return a new expression; keep the returned value.
- Read `artifact.text` for UTF-8, `artifact.bytes` for binary output, and
  `artifact.value` for the evaluated value of the source's type.

## Read only what the task needs

| Need | Reference |
| --- | --- |
| Choose a format, edit structured content, or embed a document in TOML | [Formats](references/formats.md) |
| Select dependencies from data at build time or add Python callbacks | [Lazy graphs and callbacks](references/graphs.md) |
| Count or cap output, investigate a build error, or profile a graph | [Budgets and diagnostics](references/diagnostics.md) |

For an exact method signature, check the bundled `syntwine/__init__.pyi` for
the installed version or the [stubs at v0.1.0](https://github.com/kleedaisuki/syntwine/blob/v0.1.0/python/syntwine/__init__.pyi).
