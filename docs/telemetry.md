# Native telemetry

Telemetry is off until a build selects an enabled configuration. The evaluator's default
path does not allocate a trace, queue an event, read the wall clock, or use a sink. When
enabled, the sampler makes one decision per build, rather than sampling individual
operations so that a retained trace has a coherent dependency graph. This is the same
sampling trade-off that made [Dapper](https://research.google/pubs/dapper-a-large-scale-distributed-systems-tracing-infrastructure/)
practical at high volume.

## Configuration and trace views

A build snapshots the configuration at entry: its `telemetry=` argument wins over the
context override from `telemetry.use()`, which wins over the global default. The native
extension stores the scoped override in a CPython context variable; asynchronous tasks can
use distinct configurations and newly created tasks inherit their creating context. Changing
the global setting does not change an in-flight build.

A retained build has a trace ID and an independent build ID. `Generated.trace` is a
dictionary containing the build status, spans, events, metrics, and actual dependency
edges. `MemorySink.records` exposes the same dictionaries, including failed builds.
Compare the error's `trace_id` to find the failed build. A dependency with
`discovered=True` was first selected at evaluation, not from a declared edge. Span
timestamps share a UTC anchor, while durations use a monotonic clock. A metric is a
per-build observation; instrumented operation durations may include work in child scopes.

Tracing levels control span detail: `build` keeps the terminal span, `stages` adds major
stages such as graph resolution and document composition, and `operations` adds node and
callback scopes. Metrics can remain enabled at any level. With content capture enabled, the
final successful output is exported as `content_base64`; it is instead represented by
`content_omitted_bytes` if it exceeds `max_content_bytes`.

## Queues and export

The sink retains native complete-build envelopes in a bounded queue. One build summary
(including its terminal span) consumes one slot; each additional span, event, and dependency
edge consumes another. A build's details are capped by `max_events` and sink capacity. A full
sink evicts its oldest envelopes; the view reports the per-build detail drops and a snapshot of
the sink's cumulative drops. `MemorySink.dropped_events` also includes envelope evictions.

- **Memory** exposes a snapshot immediately. `clear()` drops retained records and resets the
  drop counter.
- **JSONL** appends one complete build envelope per line.
- **SQLite** writes a transactional batch to `builds`, `spans`, `events`, and `metrics` tables.
  The `payload` column on `builds` stores the same envelope as JSONL, including dependency edges.
- **OTLP** sends [OTLP/HTTP JSON](https://opentelemetry.io/docs/specs/otlp/) using the configured
  base HTTP(S) URL and standard `/v1/traces`, `/v1/metrics`, and `/v1/logs` suffixes.
  Build IDs travel as span/log attributes; metrics use trace exemplars to avoid a new metric
  label series per build. Use `headers=` for collector authentication when needed.
  On Linux the exporter accepts `CURL_CA_BUNDLE`, `SSL_CERT_FILE`, and `SSL_CERT_DIR`,
  otherwise it looks for the host's CA bundle instead of assuming the wheel builder's
  trust-store layout. Windows and macOS use the TLS backend's system trust.
  Collector rejections are available through `rejected_records` and diagnostics through
  `last_export_warning`. Acknowledged signals (including partial success) are not replayed.

Persistent sinks perform no I/O during evaluation. Call `telemetry.flush()` to drain all
pending sinks, or `sink.flush()` to drain one sink. Flush releases the GIL while exporting.
An export error is raised independently of the original build result. Unacknowledged records
stay queued after a retryable failure while capacity allows. A remote receiver may accept data
before an acknowledgement is lost, so a retry can still deliver duplicates. Use the same build
and span IDs to correlate them. Use SQLite when different processes write to the same destination.
