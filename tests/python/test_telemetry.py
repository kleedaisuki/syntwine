from __future__ import annotations

import asyncio
import base64
import json
import sqlite3
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from _support import WorkspaceTest

from syntwine import (
    MD,
    Budget,
    BudgetExceeded,
    Document,
    OperationError,
    Source,
    Text,
    telemetry,
)
from syntwine.telemetry import (
    JSONLSink,
    MemorySink,
    OTLPSink,
    SQLiteSink,
    TelemetryConfig,
)


def config(
    sink: MemorySink | JSONLSink | OTLPSink | SQLiteSink, *, max_events: int = 65536
) -> TelemetryConfig:
    return TelemetryConfig(
        enabled=True,
        level="operations",
        traces=True,
        metrics=True,
        events=True,
        max_events=max_events,
        sink=sink,
    )


class TelemetryTests(WorkspaceTest):
    def setUp(self) -> None:
        super().setUp()
        telemetry.configure(TelemetryConfig(enabled=False))
        self.addCleanup(telemetry.configure, TelemetryConfig(enabled=False))

    def test_trace_follows_dynamic_dependency_callback_compose_post_and_budget(
        self,
    ) -> None:
        path = self.workspace / "telemetry.md"
        path.write_text("This payload is not exported by default.", encoding="utf-8")
        factories: list[str] = []

        def select(name: str) -> MD:
            factories.append(name)
            return Source(self.workspace / name).as_md()

        selected = Text(path.name).bind(select).as_text().pipe(shout)
        build = (
            Document(format="md")
            .append(Text("# Prompt"), selected)
            .squish()
            .budget(Budget(bytes=1000, tokens=100))
        )
        self.assertEqual(factories, [])
        off = build.generate(tokenizer="o200k_base")
        self.assertIsNone(off.trace)
        sink = MemorySink()
        on = build.generate(tokenizer="o200k_base", telemetry=config(sink))
        self.assertEqual(off.bytes, on.bytes)
        self.assertEqual(factories, [path.name, path.name])
        self.assertEqual(len(sink.records), 1)
        trace = sink.records[0]
        self.assertEqual(on.trace["trace_id"], trace["trace_id"])
        self.assertGreaterEqual(trace["duration_ns"], 0)
        self.assertNotIn("THIS PAYLOAD IS NOT EXPORTED BY DEFAULT", json.dumps(trace))
        self.assertNotIn("This payload is not exported by default", json.dumps(trace))

        stages = trace_names(trace)
        for name in (
            "generate",
            "graph.resolve",
            "source.read",
            "parse.md",
            "dynamic.bind",
            "custom.python",
            "document.compose",
            "document.post",
            "squish",
            "output.serialize",
            "output.validate",
            "budget.bytes",
            "budget.tokens",
        ):
            self.assertIn(name, stages)
        span_ids = {span["span_id"] for span in trace["spans"]}
        for span in trace["spans"]:
            self.assertEqual(span["trace_id"], trace["trace_id"])
            self.assertEqual(span["build_id"], trace["build_id"])
            if span["parent_span_id"]:
                self.assertIn(span["parent_span_id"], span_ids)
            self.assertGreaterEqual(span["duration_ns"], 0)
        discovered = [edge for edge in trace["dependencies"] if edge["discovered"]]
        self.assertTrue(discovered)
        executed = {span["node_id"] for span in trace["spans"]}
        self.assertTrue(all(edge["child_node_id"] in executed for edge in discovered))
        self.assertTrue(trace["metrics"])

        local = MemorySink()
        opt_in = TelemetryConfig(enabled=True, sink=local, include_content=True)
        artifact = Text("signed explicitly").generate(telemetry=opt_in)
        self.assertEqual(
            base64.b64decode(local.records[0]["content_base64"]), artifact.bytes
        )

    def test_global_context_single_build_precedence_and_async_isolation(self) -> None:
        asyncio.run(self.check_config_isolation())

    async def check_config_isolation(self) -> None:
        left, right, global_sink, single = (MemorySink() for _ in range(4))
        doc = Text("hello")
        telemetry.configure(config(global_sink))
        self.assertIsNotNone(doc.generate().trace)

        async def run(local_sink: MemorySink) -> None:
            with telemetry.use(config(local_sink)):
                await asyncio.sleep(0)
                self.assertIsNotNone(doc.generate().trace)
                self.assertIsNotNone(doc.generate(telemetry=config(single)).trace)
                self.assertIsNone(
                    doc.generate(telemetry=TelemetryConfig(enabled=False)).trace
                )

        await asyncio.gather(run(left), run(right))
        self.assertIsNotNone(doc.generate().trace)
        self.assertEqual(
            [len(sink.records) for sink in (left, right, global_sink, single)],
            [1, 1, 2, 2],
        )
        trace_ids = {
            record["trace_id"]
            for sink in (left, right, global_sink, single)
            for record in sink.records
        }
        self.assertEqual(len(trace_ids), 6)

    def test_failure_traces_flush_to_jsonl_and_sqlite(self) -> None:
        jsonl_path = self.workspace / "logs" / "events.jsonl"
        sqlite_path = self.workspace / "sql" / "telemetry.sqlite3"
        budget = Document().append(Text("é")).budget(Budget(bytes=1))
        failing = Document().append(Text("input").pipe(reject))

        with self.assertRaises(BudgetExceeded) as budget_error:
            budget.generate(telemetry=config(JSONLSink(jsonl_path)))
        with self.assertRaises(OperationError) as callback_error:
            failing.generate(telemetry=config(SQLiteSink(sqlite_path)))

        ok = (
            Text("ok")
            .bind(new_text)
            .write(self.workspace / "output", telemetry=config(SQLiteSink(sqlite_path)))
        )
        telemetry.flush()
        records = [
            json.loads(line)
            for line in jsonl_path.read_text(encoding="utf-8").splitlines()
        ]
        [trace] = records
        self.assertEqual(trace["trace_id"], budget_error.exception.trace_id)
        self.assertIn("budget.bytes", trace_names(trace))
        self.assertTrue(any(span.get("error_code") for span in trace["spans"]))
        self.assertIsInstance(callback_error.exception.cause, ValueError)
        self.assertIsNotNone(callback_error.exception.trace_id)
        with sqlite3.connect(sqlite_path) as db:
            self.addCleanup(db.close)
            for table in ("builds", "spans", "events", "metrics"):
                self.assertGreater(
                    db.execute(f"SELECT count(*) FROM {table}").fetchone()[0], 0
                )
            ids = {item[0] for item in db.execute("SELECT trace_id FROM builds")}
            self.assertIn(callback_error.exception.trace_id, ids)
        self.assertEqual((self.workspace / "output").read_bytes(), b"ok")
        if ok is not None:
            self.assertIn("write", trace_names(ok.trace))

    def test_bounded_queue_reports_drops_without_changing_output(self) -> None:
        sink = MemorySink()
        doc = Document(separator="").append(*(Text("x") for _ in range(64)))
        artifact = doc.generate(telemetry=config(sink, max_events=8))
        self.assertEqual(artifact.bytes, b"x" * 64)
        self.assertGreater(sink.records[0]["dropped_events"], 0)

    def test_native_otlp_http_delivers_trace_metrics_and_events(self) -> None:
        captured: list[tuple[str, str | None, str | None, dict[str, Any]]] = []

        class CaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                size = int(self.headers["Content-Length"])
                body = json.loads(self.rfile.read(size))
                captured.append(
                    (
                        self.path,
                        self.headers.get("Content-Type"),
                        self.headers.get("X-Run"),
                        body,
                    )
                )
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, fmt: str, *args: object) -> None:
                pass

        with ThreadingHTTPServer(("127.0.0.1", 0), CaptureHandler) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                endpoint = f"http://127.0.0.1:{server.server_port}"
                sink = OTLPSink(endpoint, headers={"X-Run": "syntwine-e2e"})
                source = Document().append(Text("one").bind(new_text)).squish()
                artifact = source.generate(telemetry=config(sink))
                telemetry.flush()
            finally:
                server.shutdown()
                worker.join(timeout=5)
        by_path = {
            path: (media, marker, body) for path, media, marker, body in captured
        }
        for path, key in (
            ("traces", "resourceSpans"),
            ("metrics", "resourceMetrics"),
            ("logs", "resourceLogs"),
        ):
            with self.subTest(path=path):
                media, marker, body = by_path[f"/v1/{path}"]
                self.assertIn("application/json", media or "")
                self.assertEqual(marker, "syntwine-e2e")
                self.assertTrue(body[key])
        spans_message = by_path["/v1/traces"][2]
        self.assertIn(artifact.trace["trace_id"], json.dumps(spans_message))

    def test_otlp_retries_only_unacknowledged_signals(self) -> None:
        received: list[str] = []
        replies = {
            "/v1/traces": (200, b'{"partialSuccess":{"rejectedSpans":"1","errorMessage":"example rejection"}}'),
            "/v1/metrics": (503, b"{}"),
        }

        class RetryHandler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                self.rfile.read(int(self.headers["Content-Length"]))
                received.append(self.path)
                status, body = replies.pop(self.path, (200, b"{}"))
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, fmt: str, *args: object) -> None:
                pass

        with ThreadingHTTPServer(("127.0.0.1", 0), RetryHandler) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                sink = OTLPSink(f"http://127.0.0.1:{server.server_port}")
                self.assertEqual(Text("hello").generate(telemetry=config(sink)).text, "hello")
                with self.assertRaisesRegex(RuntimeError, "returned HTTP 503"):
                    sink.flush()
                self.assertEqual(sink.rejected_records, 1)
                self.assertIn("example rejection", sink.last_export_warning or "")

                sink.flush()
                sink.flush()
                self.assertEqual(received, ["/v1/traces", "/v1/metrics", "/v1/metrics", "/v1/logs"])
            finally:
                server.shutdown()
                worker.join(timeout=5)


def new_text(value: str) -> Text:
    return Text(value)


def trace_names(trace: dict[str, Any]) -> set[str]:
    keys = ("name", "type", "operation")
    return {str(span.get(key)) for span in trace["spans"] for key in keys}


def shout(value: str) -> str:
    return value.upper()


def reject(value: str) -> str:
    raise ValueError(f"rejected {value}")
