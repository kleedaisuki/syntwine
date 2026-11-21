# Syntwine

Syntwine is a typed, lazy document assembly library for CPython 3.15+. File
reads, graph evaluation, parsing, document composition, final budgets, and
telemetry run in a C++23 engine. Python expressions declare the graph and
supply callbacks and build context.

## Assemble and build

```python
from syntwine import Budget, Document, MD, Source, Text

# manifest.json contains {"entry": "project.md"}.
entry = (
    Source("manifest.json").as_json().get_text("entry")
    .bind(lambda name: Source(f"prompts/{name}").as_md())
)
doc = (
    Document(format="md")
    .append(MD("# Instructions"))
    .squish()
    .append(entry)  # Still a part: squish runs after all parts are joined.
    .ensure_suffix("\n")
)
artifact = doc.write("instructions.md", budget=Budget(bytes=100_000, tokens=8_000),
                     tokenizer="o200k_base")
print(artifact.usage.tokens, artifact.sources)
```

Only `generate()` and `write()` evaluate the graph. Each call creates a new
session, so an edited source file is seen on the next build. Shared pure work
is cached within a session, not across builds. `bind` and `Source.defer`
discover dependencies during that same session; a callback should return a
source rather than calling `generate()` to build it separately.

`Source("path")` always means a file. Use `Literal(b"...")` for bytes,
`Text("...")` for text, and `MD`, `JSON`, `TOML`, and `XML` for the
corresponding inline source syntaxes. An `as_*` operation serializes its
input and interprets the bytes as the named format; it does not translate the
data model. Use `to_json()` or `to_toml()` for semantic data conversion.

There is no need to materialize an intermediate document to embed it:

```python
from syntwine import Source

config = Source("settings.toml").as_toml().set("agent.instructions", doc.as_literal())
config.write("settings.generated.toml")
```

TOML binds byte values as strictly decoded UTF-8 strings. Encode arbitrary
binary with `.base64()` or `.hex()` first. `table()` narrows a TOML view;
use `root()` to write the entire configuration after editing a table.

## Document phases

1. Evaluate and serialize the selected parts in order, including nested
   documents. Structural edits such as `insert()` and `append()` change
   these parts.
2. Join the bytes with the configured separator. The default is empty for
   raw output and a blank line for textual output.
3. Apply the whole-document post stream in declaration order. A post
   `replace()` can match across a part boundary; a post `pipe()` sees the
   whole byte string.
4. Validate the final format and budgets. `write()` replaces the output file
   only after a successful build.

`squish()` uses the document format unless given an explicit mode. In plain
text it collapses horizontal whitespace, keeps line breaks, and retains at
most one blank line between paragraphs. Markdown keeps code and block
structure; XML protects mixed content and `xml:space`; JSON strips whitespace
outside strings without changing their spelling or that of numbers. TOML is
validated without changing its layout.

`Generated.value` retains the source's value type; `.bytes` is the final
serialized artifact, and `.text` strictly decodes it as UTF-8. Character
budgets count Unicode code points; byte budgets cover arbitrary binary. Token
budgets apply to the final text, not the sum of counts taken from individual
parts. Built-in tokenizer names are `o200k_base`, `o200k_harmony`,
`cl100k_base`, `p50k_base`, `p50k_edit`, `r50k_base`, and its alias
`gpt2`. Vocabulary data is supplied by the package. Text resembling a
special token is counted as ordinary content.

## Python operations

```python
from syntwine import Text, operation, ops

@operation(input=Text, output=Text, pure=True, version="1")
def clean(value: str) -> str:
    return "\n".join(line.rstrip() for line in value.splitlines())

ops.mount(Text, "clean", clean)
text = Text("Hello  ").pipe(clean)    # Statically typed
same = Text("Hello  ").op("clean")    # Named lookup
```

Native nodes call the registered Python function with the evaluated value, not
a source. Mark operations that observe or modify external state as
`pure=False`; `tap()` is an observation and is never cached. Use `map()`
for an ordinary type-changing callback and `bind()` for a callback that
returns a source.

Type checkers infer values from `Source[T]` and `Generated[T]`, including
the output of `map()`, `bind()`, and typed operations. Use `.pipe(clean)`
for static completion; attributes added by `ops.mount()` are runtime only.

For a custom format, define a `Source[Value]` subclass and give the native
registry its parse, serialize, and optional validation callbacks:

```python
from syntwine import Source, ops

class Lines(Source[list[str]]):
    pass

def parse_lines(data: bytes) -> list[str]:
    return data.decode("utf-8").splitlines()

def serialize_lines(lines: list[str]) -> bytes:
    return "\n".join(lines).encode("utf-8")

ops.register_format(Lines, parse_lines, serialize_lines)
lines = Source("names.txt").as_format(Lines)
```

## Diagnostics and telemetry

Build errors carry an operation and dependency path, and include a location
or original callback cause when available. `source.inspect()` describes the
declared graph without evaluating it. An enabled trace describes the graph
that actually ran, including dynamically discovered edges. It is available
as `artifact.trace` on success and `error.trace` on failure.

```python
from syntwine import telemetry
from syntwine.telemetry import JSONLSink, TelemetryConfig

with telemetry.use(TelemetryConfig(enabled=True, sink=JSONLSink("builds.jsonl"))):
    artifact = doc.generate()
    telemetry.flush()
```

Telemetry is off by default. A per-build `telemetry=` argument overrides a
task-local `telemetry.use(...)` context, which overrides the process default
set by `telemetry.configure(...)`. Other sinks are `MemorySink`,
`SQLiteSink`, and the HTTP(S) `OTLPSink`. Only metadata is recorded by
default; content collection requires explicit opt-in. See the
[telemetry guide](docs/telemetry.md) for setup, sink behavior, and trace
interpretation.

## Develop

### Repository layout

| Path | Contents |
| --- | --- |
| `include/syntwine/detail/` | Internal C++ headers; not installed as a public C++ SDK |
| `src/` | Native implementations, grouped into bindings, core, formats, platform, and telemetry |
| `python/syntwine/` | Installable Python package, type stubs, and tokenizer data |
| `tests/native/`, `tests/python/` | C++ regression tests and installed-wheel Python tests, respectively |
| `cmake/`, `benchmarks/`, `docs/` | Build dependencies, performance workloads, and design notes |

### Build and test a wheel

Use a C++23 compiler, CMake, and CPython 3.15+. Linux source builds also need
OpenSSL 3 development headers for OTLP over HTTPS. CMake downloads pinned native
dependencies during a source build. Build and install a wheel, then run the
integration tests against that installed artifact:

```console
python -m pip install build
python -m build
python -m pip install --force-reinstall dist/<wheel-filename>
python -P -m unittest discover -s tests/python -v
```

To check the consumer typing examples, run
`mypy --strict --python-version 3.15 tests/python/typing_contract.py`.

### Native development with CMake and clangd

From the repository root, with uv and Ninja installed (skip `uv venv` if
the checkout already has a CPython 3.15+ virtual environment):

```sh
uv venv --python 3.15
uv pip install pybind11==3.1.0
cmake --preset dev
```

CMake prefers an active virtual environment, with the checkout's `.venv` as a
hint when configuring from an IDE or an unactivated shell. CMake finds
pybind11 in the selected Python environment if it is not already
available through CMake's package search. The preset generates
`build/compile_commands.json`, which clangd discovers automatically. Configuring
is enough for editor support; no extension build or symlink is needed.

Run `cmake --build --preset dev` to build the native extension and run the
native regression tests. Use `ctest --preset dev` to rerun them without rebuilding.
Keep machine-specific overrides in the Git-ignored `CMakeUserPresets.json`.

### Format C++

Use clang-format 22.1.8 with the checked-in `.clang-format` configuration.
From a POSIX shell (Git Bash on Windows), run:

```sh
git ls-files -z -- '*.cpp' '*.hpp' | xargs -0 uvx clang-format@22.1.8 -i
```

CI checks the same files with `--dry-run --Werror`.

See the [native architecture notes](docs/architecture.md) for design
decisions and [GOAL.md](GOAL.md) for the API contract and acceptance matrix.
