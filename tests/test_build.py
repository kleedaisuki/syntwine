from __future__ import annotations

import hashlib
import subprocess
import sys
import tomllib
import unittest
from collections.abc import Mapping
from textwrap import dedent

from _support import WorkspaceTest

from syntwine import (
    MD,
    Budget,
    BudgetExceeded,
    BuildOptions,
    CycleError,
    DecodeError,
    Document,
    Literal,
    OperationConflict,
    OperationError,
    ParseError,
    Source,
    SourceError,
    Text,
    operation,
    ops,
)
from syntwine.telemetry import TelemetryConfig


class BuildTests(WorkspaceTest):
    def test_lazy_manifest_and_nested_document_embedded_in_toml(self) -> None:
        manifest = self.workspace / "manifest.json"
        selected = self.workspace / "règles.md"
        other = self.workspace / "other.md"
        settings = self.workspace / "settings.toml"
        openings: list[str] = []

        def open_rule(name: str) -> MD:
            openings.append(name)
            return Source(self.workspace / name).as_md()

        rule = Source(manifest).as_json().get_text("entry").bind(open_rule)
        doc = (
            Document(format="md")
            .append(Text("# Rules"), rule)
            .squish(mode="markdown")
            .normalize_newlines("lf")
            .ensure_suffix("\n")
        )
        config = (
            Source(settings)
            .as_toml()
            .set("agent.instructions", doc.as_literal())
            .set("agent.enabled", True)
        )
        self.assertEqual(openings, [])
        self.assertFalse(manifest.exists())
        self.assertFalse(settings.exists())

        manifest.write_text('{"entry":"règles.md"}', encoding="utf-8")
        selected.write_text("## Selected\n\n- café", encoding="utf-8")
        settings.write_text(
            "# managed configuration\n[agent]\nmode = 'local'\n", encoding="utf-8"
        )
        artifact = config.generate(budget=Budget(bytes=1000))
        agent = tomllib.loads(artifact.text)["agent"]
        self.assertEqual(agent["instructions"], "# Rules\n\n## Selected\n\n- café\n")
        self.assertEqual(agent["mode"], "local")
        self.assertTrue(agent["enabled"])
        self.assertIn("# managed configuration", artifact.text)
        self.assertEqual(openings, ["règles.md"])
        used = str(artifact.sources)
        for path in (manifest, selected, settings):
            self.assertIn(path.name, used)
        self.assertNotIn(other.name, used)

        manifest.write_text('{"entry":"other.md"}', encoding="utf-8")
        other.write_text("## Changed", encoding="utf-8")
        self.assertIn(
            "## Changed", tomllib.loads(config.generate().text)["agent"]["instructions"]
        )
        self.assertEqual(openings, ["règles.md", "other.md"])

    def test_document_barrier_nested_post_and_immutable_parts(self) -> None:
        seen: list[bytes] = []

        def banner(payload: bytes) -> bytes:
            seen.append(payload)
            return b"<" + payload + b">"

        inner = Document(format="raw").append(Literal(b" A"), Literal(b"B ")).strip()
        prefix = Document(format="raw").append(inner).replace(b"BC", b"X").pipe(banner)
        final = prefix.append(Literal(b"C"))
        self.assertIsInstance(prefix, Source)
        self.assertIsInstance(final, Document)
        self.assertEqual(seen, [])
        self.assertEqual(final.generate().bytes, b"<AX>")
        self.assertEqual(prefix.generate().bytes, b"<AB>")
        self.assertEqual(seen, [b"AX", b"AB"])

        parts = Document(format="raw").append(Literal(b"0"), Literal(b"2"))
        old = parts
        parts += Literal(b"3")
        edited = parts.insert(1, Literal(b"1")).replace_part(2, Literal(b"X"))
        self.assertEqual(old.generate().bytes, b"02")
        self.assertEqual(parts.generate().bytes, b"023")
        self.assertEqual(edited.slice_parts(1, 4).remove(1).generate().bytes, b"13")

    def test_pure_cache_and_ordered_effects_in_each_session(self) -> None:
        pure_calls: list[str] = []
        tapped: list[str] = []

        @operation(input=Text, output=Text, pure=True, version="1")
        def uppercase(value: str) -> str:
            pure_calls.append(value)
            return value.upper()

        shared = Text("one").pipe(uppercase)
        effect = shared.tap(tapped.append).as_text()
        query = effect.combine(effect, shared, fn=combine_words)
        self.assertEqual(pure_calls, [])
        self.assertEqual(query.generate().text, "ONE,ONE,ONE")
        self.assertEqual(pure_calls, ["one"])
        self.assertEqual(tapped, ["ONE", "ONE"])
        self.assertEqual(query.generate().text, "ONE,ONE,ONE")
        self.assertEqual(pure_calls, ["one", "one"])
        self.assertEqual(tapped, ["ONE", "ONE", "ONE", "ONE"])

    def test_large_incremental_document_keeps_prior_snapshot(self) -> None:
        doc = Document(separator="|")
        values = [str(index) for index in range(133)]
        for value in values[:65]:
            doc += Text(value)
        previous = doc
        for value in values[65:]:
            doc += Text(value)
        self.assertEqual(previous.generate().text, "|".join(values[:65]))
        self.assertEqual(doc.generate().text, "|".join(values))
        declared = doc.inspect()
        root = next(
            node for node in declared["nodes"] if node["node_id"] == declared["root"]
        )
        # The dynamic separator is also a Source; never mistake it for an additional Part.
        self.assertGreaterEqual(len(root["dependencies"]), len(values))

    def test_mounted_operations_and_source_arguments(self) -> None:
        @operation(input=Text, output=Text, pure=True, version="1")
        def surround(value: str, ending: str) -> str:
            return value + ending

        alias = "build_test_surround"
        ops.mount(Text, alias, surround, overwrite=True)
        with self.assertRaises(OperationConflict):
            ops.mount(Text, alias, surround)
        suffix = Source.defer(compute_suffix)
        fragment = Text("{who}").format(who=Text("Klee")).op(alias, suffix)
        named = getattr(Text("Hi"), alias)(Text("!"))
        piped = Text("Hi").pipe(surround, Text("?"))
        self.assertEqual(fragment.generate().text, "Klee!")
        self.assertEqual(named.generate().text, "Hi!")
        self.assertEqual(piped.generate().text, "Hi?")
        self.assertEqual(
            Text("Hi").zip(suffix).map(combine_pair).generate().text, "Hi!"
        )

    def test_part_filtering_and_deferred_context_remain_lazy(self) -> None:
        calls: list[tuple[str, str]] = []

        def choose_project(context: Mapping[str, object]) -> Text:
            project = str(context["project"])
            calls.append(("defer", project))
            return Text(project)

        def keep_part(value: str) -> bool:
            calls.append(("filter", value))
            return value != "skip"

        def label_part(value: str) -> Text:
            calls.append(("map", value))
            return Text("[" + value + "]")

        doc = (
            Document(separator=", ")
            .append(Text("skip"), Source.defer(choose_project), Text("footer"))
            .filter_parts(keep_part)
            .map_parts(label_part)
        )
        self.assertEqual(calls, [])
        self.assertEqual(
            doc.generate(context={"project": "syntwine"}).text, "[syntwine], [footer]"
        )
        self.assertNotIn(("map", "skip"), calls)
        self.assertIn(("defer", "syntwine"), calls)

    def test_registered_format_joins_the_same_build(self) -> None:
        class Score(Source[int]):
            pass

        parse_calls: list[bytes] = []

        def parse_score(payload: bytes) -> int:
            parse_calls.append(payload)
            return int(payload.removeprefix(b"score:"))

        def serialize_score(value: int) -> bytes:
            return f"score:{value}".encode("ascii")

        def valid_score(value: int) -> bool:
            return value >= 0

        ops.register_format(Score, parse_score, serialize_score, validate=valid_score)

        @operation(input=Score, output=Score, pure=True)
        def boost(value: int) -> int:
            return value + 1

        ops.mount(Score, "build_test_boost", boost)
        score = Literal(b"score:8").as_format(Score).pipe(boost).op("build_test_boost")
        doc = Document(separator="=").append(Text("result"), score)
        self.assertEqual(parse_calls, [])
        self.assertIsInstance(score, Score)
        self.assertEqual(doc.generate().text, "result=score:10")
        self.assertEqual(parse_calls, [b"score:8"])
        self.assertEqual(score.generate().value, 10)
        with self.assertRaises(ParseError):
            Literal(b"score:-1").as_format(Score).generate()

    def test_literal_bytes_and_strict_text(self) -> None:
        sample = b"\x00\xffKlee\x00"
        raw = Literal(sample)
        self.assertIsInstance(raw, Source)
        self.assertEqual(raw.generate().value, sample)
        self.assertEqual(raw.generate().bytes, sample)
        self.assertIsNone(raw.generate().usage.characters)
        with self.assertRaises(DecodeError):
            raw.as_text().generate()
        with self.assertRaises(DecodeError):
            _ = raw.generate().text
        self.assertEqual(raw.slice(1, -1).hex().generate().text, "ff4b6c6565")
        self.assertEqual(raw.base64().generate().text, "AP9LbGVlAA==")
        self.assertEqual(
            raw.sha256().generate().text, hashlib.sha256(sample).hexdigest()
        )
        self.assertEqual(Literal(b"caf\xe9").as_text("latin-1").generate().text, "café")
        self.assertEqual(Text("🧪hé").slice(1, None).generate().text, "hé")

    def test_exact_final_budgets_and_atomic_write(self) -> None:
        dest = self.workspace / "résultat.txt"
        dest.write_bytes(b"original")
        doc = Document(separator="").append(Text("h"), Text("ello"))
        self.assertEqual(doc.generate(tokenizer="o200k_base").usage.tokens, 1)
        between = doc.join(" ")
        self.assertEqual(between.generate(tokenizer="o200k_base").usage.tokens, 2)
        with self.assertRaises(BudgetExceeded) as error:
            between.write(dest, budget=Budget(tokens=1), tokenizer="o200k_base")
        self.assertEqual(error.exception.actual, 2)
        self.assertEqual(error.exception.budget, 1)
        self.assertEqual(error.exception.overage, 1)
        self.assertEqual(dest.read_bytes(), b"original")
        self.assertEqual(list(self.workspace.iterdir()), [dest])

        artifact = Text("Aé💙文").generate(budget=Budget(characters=4, bytes=10))
        self.assertEqual(artifact.usage.characters, 4)
        self.assertEqual(artifact.usage.bytes, 10)
        limited = doc.budget(Budget(bytes=5))
        with self.assertRaises(BudgetExceeded):
            limited.append(Text("!")).write(dest)
        with self.assertRaises(DecodeError):
            Literal(b"\xff").generate(budget=Budget(characters=1))
        doc.write(dest, budget=Budget(tokens=1), tokenizer="o200k_base")
        self.assertEqual(dest.read_bytes(), b"hello")

    def test_tokenizer_reference_vectors_have_no_runtime_dependency(self) -> None:
        # tiktoken 0.14.0 encode_ordinary: special-looking inputs are ordinary text.
        cases = (
            ("We're WORLD! 123456.\r\n你好，世界 🌸🫶\u200d👩🏽\0", (21, 29, 34, 34)),
            ("\u180e-f", (5, 5, 5, 5)),  # U+180E is not Unicode White_Space.
            ("\U0001e6c0-f", (6, 6, 6, 6)),  # Newly assigned in Unicode 17, not 16.
            ("<|endoftext|>", (7, 7, 7, 7)),
            ("abcdefgh" * 16, (16, 16, 48, 48)),  # Long single pretokenization piece.
        )
        encodings = ("o200k_base", "cl100k_base", "r50k_base", "p50k_base")
        for body, counts in cases:
            for encoding, count in zip(encodings, counts, strict=True):
                with self.subTest(body=body, encoding=encoding):
                    artifact = Text(body).generate(
                        tokenizer=encoding, budget=Budget(tokens=count)
                    )
                    self.assertEqual(artifact.usage.tokens, count)

    def test_dynamic_cycle_and_budgeted_expansion(self) -> None:
        def back(ctx: Mapping[str, object]) -> Source[bytes]:
            return cycle

        cycle = Source.defer(back).as_literal()
        with self.assertRaises(CycleError) as error:
            cycle.generate()
        path = error.exception.dependency_path
        self.assertGreaterEqual(len(path), 3)
        self.assertEqual(path[0]["node_id"], path[-1]["node_id"])

        def expand(ctx: Mapping[str, object]) -> Source[bytes]:
            return Source.defer(expand).as_literal()

        with self.assertRaises(OperationError) as too_deep:
            Source.defer(expand).generate(options=BuildOptions(max_depth=5))
        self.assertGreaterEqual(len(too_deep.exception.dependency_path), 5)

    def test_operation_and_source_errors_preserve_diagnostics(self) -> None:
        absent = self.workspace / "absent.txt"
        build = Document().append(Source(absent).as_text()).strip()
        with self.assertRaises(SourceError) as missing:
            build.generate()
        self.assertIn(absent.name, str(missing.exception.source_id))
        self.assertGreaterEqual(len(missing.exception.dependency_path), 2)

        failing = Document().append(Text("entry").pipe(fail_in_python))
        with self.assertRaises(OperationError) as raised:
            failing.generate()
        error = raised.exception
        self.assertIsInstance(error.cause, LookupError)
        self.assertIn("test callback", str(error.cause))
        self.assertGreaterEqual(len(error.dependency_path), 2)

        def reenter(value: str) -> str:
            return Text(value).generate().text

        with self.assertRaises(OperationError):
            Text("x").pipe(reenter).generate()

    def test_failure_trace_is_available_without_a_sink(self) -> None:
        source = Document().append(Text("entry").pipe(fail_in_python))
        with self.assertRaises(OperationError) as raised:
            source.generate(telemetry=TelemetryConfig(enabled=True))
        error = raised.exception
        self.assertEqual(error.trace["trace_id"], error.trace_id)
        self.assertEqual(error.trace["status"], "error")
        self.assertTrue(
            any(
                span["name"] == "custom.python" and span["status"] == "error"
                for span in error.trace["spans"]
            )
        )

    def test_large_depth_limit_cannot_overrun_native_stack(self) -> None:
        program = dedent(
            """
            from collections.abc import Mapping
            from syntwine import BuildOptions, EvaluationLimit, Source

            def expand(context: Mapping[str, object]) -> Source[bytes]:
                return Source.defer(expand).as_literal()

            options = BuildOptions(max_depth=1_000_000, max_nodes=1_000_000)
            try:
                Source.defer(expand).generate(options=options)
            except EvaluationLimit as error:
                assert error.dependency_path
            else:
                raise AssertionError("unbounded expansion unexpectedly generated an output")
            """
        )
        child = subprocess.run(
            [sys.executable, "-c", program],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertEqual(child.returncode, 0, child.stdout + child.stderr)

    def test_inspect_is_static_and_build_is_native(self) -> None:
        file_node = Source(self.workspace / "later.md").as_md()
        build = Document().append(file_node).squish()
        declared = build.inspect()
        ids = {node["node_id"] for node in declared["nodes"]}
        self.assertIn(declared["root"], ids)
        self.assertGreaterEqual(len(ids), 3)
        self.assertTrue(
            all(
                dep in ids for node in declared["nodes"] for dep in node["dependencies"]
            )
        )
        self.assertTrue(Source.__module__.endswith("_syntwine_native"))


def combine_words(first: str, second: str, third: str) -> str:
    return f"{first},{second},{third}"


def combine_pair(pair: tuple[str, str]) -> str:
    return "".join(pair)


def compute_suffix(ctx: Mapping[str, object]) -> str:
    return "!"


def fail_in_python(value: str) -> str:
    raise LookupError(f"test callback rejected {value}")


if __name__ == "__main__":
    unittest.main()
