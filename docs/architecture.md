# Native execution and distribution

## State and evaluation

A Source is a handle to a shared, immutable native graph node. A compound expression owns its
operands; it never holds a reference to the session that evaluates it. The session owns context,
node visitation, memoized values, and per-build diagnostics. Encountering a new Source through
a callback adds an edge to the *current* session and visits the dependency before resuming the
caller. This follows the suspending scheduler described in
[Build Systems à la Carte](https://simon.peytonjones.org/assets/pdfs/build-systems-jfp.pdf);
unlike a build cache, the memo table is deliberately not reused across input snapshots.

Purity propagates through dependencies actually visited. A pure expression downstream of an
impure node is not memoized, including when the impure dependency is discovered dynamically.
Evaluation enforces the configured node and depth limits as well as thread stack headroom,
using the platform stack range and [CPython's native recursion guard](https://docs.python.org/3.15/c-api/exceptions.html#recursion-control).
On Windows the effective evaluation depth is also capped at 256 to leave room for native
exception unwinding; a lower user-configured limit takes precedence.
Dropping the last reference to a deep static graph drains its nodes iteratively; it does not
require a call stack as deep as the declaration graph.

A Document separates its ordered Parts from its Post Stream. Structural methods edit only the
Parts; previously declared post operations are applied to the new complete composition.
Parts are stored in an immutable forest of bounded leaves. Appending shares completed chunks
with earlier versions, and evaluation visits them in order without flattening. Inspection and
telemetry enumerate the declaration edges only when needed. Arbitrary positional edits may
materialize and rebuild the sequence.
Converting a Document into another Source introduces a dependency on its final bytes, not on its
pre-join fragments. For example, embedding generated Markdown into TOML passes the finished
bytes through the TOML UTF-8 string boundary before serialization.

CPython callbacks consume already evaluated values. Native operations release the GIL where
they only work on native data and do not manipulate Python reference counts. A fresh terminal
call is not a way to resolve dependencies inside a callback: use `bind` or `defer` and return the
Source instead.

## Markdown and XML representations

Markdown keeps its original UTF-8 buffer. Structural queries parse a
[CommonMark tree](https://github.com/commonmark/cmark) on first use and edit original source
ranges rather than rendering the tree. In particular, heading-looking lines inside fences or
indented code are not edits. Whole-document Markdown squish only collapses blank gaps between
independent top-level blocks; it does not rewrite intra-block whitespace or gaps containing
unrepresented constructs such as link definitions.

XML parses with [libxml2](https://gnome.pages.gitlab.gnome.org/libxml2/), with external entity
loading disabled. Whole-tree output preserves references to external entities; a detached
fragment that still depends on their declarations is a serialization error. The value keeps
the parsed tree and the original payload. XPath selects an
ordered view over that same tree; edits copy only its active forest and never mutate the input.
Unmodified values preserve the original byte encoding. Structural edits serialize as UTF-8,
and extracted fragments carry namespace declarations needed to stand alone. Passing Text
or bytes to an XML constructor creates text nodes; only explicit XML operands insert markup.
Markup-only squish never treats inferred indentation as disposable text: absent a declared
element-only content model, whitespace between elements is preserved. The explicit
`element_only` policy also removes indentation from elements with no mixed text, while
respecting `xml:space`.

## JSON and TOML representations

JSON keeps the original validated source alongside a native ordered value tree from
[jsoncons](https://github.com/danielaparker/jsoncons). Structural edits operate on the
tree and JSONPath queries run natively. Minifying unmodified source strips only JSON
whitespace outside string literals, retaining number spellings, escapes, and member order.

TOML stores a [toml++](https://marzer.github.io/tomlplusplus/) tree and source buffer.
A table view shares the root and carries a key path, so subtable edits can still return
the complete document via `root()`. Edits use parser-reported key and value ranges to
splice the original text, mapping character columns to byte offsets. The result is
reparsed and compared with the edited tree before publication. Canonical serialization
is the fallback for changes that cannot preserve the original layout; source comments
remain available even in that fallback. Passing a binary Source to a TOML value position
means strict UTF-8 decoding to a TOML string, not inserting TOML syntax.

Reinterpretation always goes through serialized bytes. Explicit `to_json()` and
`to_toml()` instead convert the native trees; TOML dates and times become ISO strings
in JSON, and values with no target representation (e.g. JSON null in TOML) fail.

## Exact final-output budgets

The UTF-8 validator counts Unicode scalar values, rejects overlong encodings and surrogates,
and operates on the actual serialized bytes. Binary content can have byte usage without
character or token usage. A Document Post Stream can change both UTF-8 validity and token
boundaries; no budget accumulates estimates from individual Parts.

The built-in tokenizers implement ordinary-text byte pair encoding (BPE) with the
[upstream tiktoken patterns and rank tables](https://github.com/openai/tiktoken/blob/main/tiktoken_ext/openai_public.py).
[Precompiled vocabulary indexes](tokenizer-vocabularies.md#compiled-index) ship in the wheel
and are loaded only for the named encoding; terminal calls do not download data.
PCRE2 is pinned to the Unicode property version used by the reference
tokenizer (Unicode 16), and whitespace follows that version's `White_Space` property.
Strings resembling control tokens are counted as ordinary input text. Values for
`gpt2`/`r50k_base`, `p50k_base`/`p50k_edit`, and `o200k_base`/`o200k_harmony` share
rank tables because their differences are in control tokens under these semantics.

For short pieces a stack buffer stores merge boundaries; longer pieces use a min-heap with
leftmost tie-breaking and an indexed linked list. Merging invalidates only adjacent candidates.
The formal distinction between a ranked merge algorithm and a longest-match tokenizer is
discussed in [Formalizing BPE Tokenization](https://arxiv.org/abs/2309.08715).

## File output and distributions

The writer creates an exclusive sibling temporary file, writes and closes it, then performs a
same-filesystem rename (POSIX) or replacement (Windows). A failed build never opens the output
path. A failed write removes its temporary file. This is atomic publication, not a promise of
crash durability across power loss.

The CMake build pins dependency sources by SHA-256; the generated wheel contains one CPython
extension, vocabulary data, third-party notices, and typing metadata. The Linux wheel uses
manylinux 2.34 and wheel repair for non-system runtime libraries (including OpenSSL 3).
Windows uses Schannel for
OTLP HTTPS instead of shipping a parallel system trust store. macOS uses the SDK's HTTP client.
Only the standard CPython 3.15 ABI is built by the current release matrix. GitHub Actions
builds the wheels from the sdist and runs native checks and Python integration tests against
the installed wheels on Linux, Windows, and macOS.
