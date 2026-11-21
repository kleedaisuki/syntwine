"""Run Syntwine's representative build workloads on the installed wheel."""

from __future__ import annotations

import argparse
import json
import platform
import statistics
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from tempfile import TemporaryDirectory
from time import perf_counter_ns
from typing import TypedDict

from syntwine import BuildOptions, Document, Source, Text
from syntwine.telemetry import MemorySink, TelemetryConfig


class Case(TypedDict):
    name: str
    per_sample: int
    detail: str
    run: Callable[[], object]


class Result(TypedDict):
    name: str
    detail: str
    per_sample: int
    median_ns: int
    samples_ns: list[int]


def run_case(case: Case, repeat: int, warmup: int) -> Result:
    run = case["run"]
    samples: list[int] = []
    for _ in range(warmup):
        run()
    for _ in range(repeat):
        start = perf_counter_ns()
        for _ in range(case["per_sample"]):
            run()
        samples.append((perf_counter_ns() - start) // case["per_sample"])
    return {
        "name": case["name"],
        "detail": case["detail"],
        "per_sample": case["per_sample"],
        "median_ns": int(statistics.median(samples)),
        "samples_ns": samples,
    }


def make_parts(part_count: int) -> Document:
    labels = [Text(str(index)).format() for index in range(part_count)]
    return Document().append(*labels).strip()


def append_one_at_a_time(parts: list[Text]) -> Document:
    doc = Document()
    for part in parts:
        doc += part
    return doc


def dynamic_part(index: str) -> Text:
    return Text("part " + index)


def callback(value: str) -> str:
    return value.upper()


def make_cases(workspace: Path, part_count: int, repetitions: int) -> list[Case]:
    json_file = workspace / "large.json"
    json_file.write_text(
        json.dumps(
            [
                {"name": "clause " + str(index), "value": index}
                for index in range(repetitions)
            ]
        ),
        encoding="utf-8",
    )
    toml_file = workspace / "large.toml"
    toml_file.write_text(
        "".join(f"[clause{index}]\nvalue = {index}\n" for index in range(repetitions)),
        encoding="utf-8",
    )
    md_file = workspace / "large.md"
    md_file.write_text(
        (
            "## Clause\n\n- Read the contract.   \n  Keep this indentation.\n\n"
            "```python\nline = 'do not edit'\n```\n\n"
        )
        * repetitions,
        encoding="utf-8",
    )
    xml_file = workspace / "large.xml"
    xml_file.write_text(
        "<rules>" + "<rule score='3'>keep</rule>" * repetitions + "</rules>",
        encoding="utf-8",
    )
    small = make_parts(8)
    large_json, large_toml = Source(json_file).as_json(), Source(toml_file).as_toml()
    large_xml = Source(xml_file).as_xml()
    large_md = Source(md_file).as_md()
    assembled_md = Document(format="md").append(large_md).squish()
    token_payload = Text("Build α, café, 🌷, 你好, 2026-11-21.\n" * repetitions)
    parts = [Text(str(index)) for index in range(part_count)]
    dynamic = Document(separator="\n").append(
        *(Text(str(index)).bind(dynamic_part) for index in range(part_count))
    )
    callbacks = Document(separator="").append(
        *(Text("note").pipe(callback) for _ in range(part_count))
    )
    sink = MemorySink(capacity=8192)
    on = TelemetryConfig(enabled=True, sink=sink, level="operations")
    off = TelemetryConfig(enabled=False)
    options = BuildOptions(max_nodes=max(100000, part_count * 6))
    process = [sys.executable, "-c", "import syntwine"]

    return [
        {
            "name": "startup",
            "detail": "New CPython process and native extension import",
            "per_sample": 1,
            "run": lambda: subprocess.run(process, cwd=workspace, check=True),
        },
        {
            "name": "construct-small",
            "detail": "Assemble eight literal/format Parts and one post operation",
            "per_sample": 200,
            "run": lambda: make_parts(8),
        },
        {
            "name": "construct-fanout",
            "detail": f"Assemble {part_count} prebuilt Parts using += in a Python loop",
            "per_sample": 1,
            "run": lambda: append_one_at_a_time(parts),
        },
        {
            "name": "extend-fanout",
            "detail": f"Assemble the same {part_count} prebuilt Parts in one extend call",
            "per_sample": 1,
            "run": lambda: Document().extend(parts),
        },
        {
            "name": "generate-small",
            "detail": "Generate an existing eight-Part Document",
            "per_sample": 200,
            "run": small.generate,
        },
        {
            "name": "parse-json",
            "detail": f"Read, parse, and serialize {json_file.stat().st_size} bytes of JSON",
            "per_sample": 1,
            "run": large_json.generate,
        },
        {
            "name": "parse-toml",
            "detail": f"Read, parse, and serialize {toml_file.stat().st_size} bytes of TOML",
            "per_sample": 1,
            "run": large_toml.generate,
        },
        {
            "name": "parse-xml",
            "detail": f"Read, parse, and serialize {xml_file.stat().st_size} bytes of XML",
            "per_sample": 1,
            "run": large_xml.generate,
        },
        {
            "name": "squish-markdown",
            "detail": f"Read and squish a structured Markdown Document ({md_file.stat().st_size} bytes)",
            "per_sample": 1,
            "run": assembled_md.generate,
        },
        {
            "name": "count-tokens",
            "detail": "Exact warm o200k_base count on final multilingual UTF-8 text",
            "per_sample": 1,
            "run": lambda: token_payload.generate(tokenizer="o200k_base"),
        },
        {
            "name": "dynamic-fanout",
            "detail": f"{part_count} Parts, each discovers its own Text through bind",
            "per_sample": 1,
            "run": lambda: dynamic.generate(options=options),
        },
        {
            "name": "python-callbacks",
            "detail": f"{part_count} bytes-producing Parts with Python pipe",
            "per_sample": 1,
            "run": lambda: callbacks.generate(options=options),
        },
        {
            "name": "telemetry-off",
            "detail": "Same small Document, explicit no-op telemetry",
            "per_sample": 30,
            "run": lambda: small.generate(telemetry=off),
        },
        {
            "name": "telemetry-on",
            "detail": "Same small Document, full native trace into a bounded MemorySink",
            "per_sample": 30,
            "run": lambda: small.generate(telemetry=on),
        },
    ]


def positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeat", type=positive, default=7)
    parser.add_argument("--warmup", type=positive, default=2)
    parser.add_argument("--parts", type=positive, default=1000)
    parser.add_argument("--rows", type=positive, default=10000)
    parser.add_argument(
        "--case", action="append", default=[], help="Run only the named case(s)."
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    scratch = Path(__file__).resolve().parents[1] / ".temp"
    scratch.mkdir(exist_ok=True)
    with TemporaryDirectory(prefix="benchmark-", dir=scratch) as raw:
        cases = make_cases(Path(raw), args.parts, args.rows)
        unknown: set[str] = set(args.case) - {case["name"] for case in cases}
        if unknown:
            parser.error(f"unknown case: {', '.join(sorted(unknown))}")
        results = [
            run_case(case, args.repeat, args.warmup)
            for case in cases
            if not args.case or case["name"] in args.case
        ]
    report = json.dumps(
        {
            "platform": platform.platform(),
            "python": sys.version,
            "repeat": args.repeat,
            "warmup": args.warmup,
            "results": results,
        },
        indent=2,
    )
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
    else:
        print(report)


if __name__ == "__main__":
    main()
