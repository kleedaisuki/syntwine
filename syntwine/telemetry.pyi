from collections.abc import Mapping
from contextlib import AbstractContextManager
from os import PathLike
from pathlib import Path
from typing import Literal, NotRequired, TypedDict

type TelemetryLevel = Literal["build", "stages", "operations"]

class SpanRecord(TypedDict):
    trace_id: str
    build_id: str
    span_id: str
    parent_span_id: str | None
    node_id: int | None
    source_id: str | None
    operation: str
    name: str
    type: Literal["span", "event"]
    status: Literal["ok", "error"]
    error_code: str | None
    start_ns: int
    duration_ns: int
    input_bytes: int | None
    output_bytes: int | None
    characters: int | None
    tokens: int | None
    cache_hit: bool | None
    discovered_dependencies: int | None
    dependency_node_id: NotRequired[int]

class Dependency(TypedDict):
    parent_node_id: int
    child_node_id: int
    discovered: bool

class Trace(TypedDict):
    trace_id: str
    build_id: str
    type: Literal["build"]
    status: Literal["ok", "error"]
    start_ns: int
    duration_ns: int
    output_bytes: int | None
    error_code: str | None
    spans: list[SpanRecord]
    events: list[SpanRecord]
    metrics: dict[str, int]
    dependencies: list[Dependency]
    dropped_events: int
    sink_dropped_events: int
    content_base64: NotRequired[str]
    content_omitted_bytes: NotRequired[int]

class Sink:
    @property
    def capacity(self) -> int: ...
    @property
    def dropped_events(self) -> int: ...
    def flush(self) -> None: ...

class MemorySink(Sink):
    def __init__(self, capacity: int = ...) -> None: ...
    @property
    def records(self) -> list[Trace]: ...
    def snapshot(self) -> list[Trace]: ...
    def clear(self) -> None: ...

class JSONLSink(Sink):
    def __init__(self, path: str | PathLike[str], capacity: int = ...) -> None: ...
    @property
    def path(self) -> Path: ...

class SQLiteSink(Sink):
    def __init__(self, path: str | PathLike[str], capacity: int = ...) -> None: ...
    @property
    def path(self) -> Path: ...

class OTLPSink(Sink):
    def __init__(
        self,
        endpoint: str,
        headers: Mapping[str, str] | None = None,
        timeout: float = 5.0,
        capacity: int = ...,
    ) -> None: ...
    @property
    def endpoint(self) -> str: ...
    @property
    def timeout(self) -> float: ...
    @property
    def rejected_records(self) -> int: ...
    @property
    def last_export_warning(self) -> str | None: ...

class TelemetryConfig:
    def __init__(
        self,
        *,
        enabled: bool = False,
        traces: bool = True,
        metrics: bool = True,
        events: bool = True,
        level: TelemetryLevel = "operations",
        sample_rate: float = 1.0,
        include_content: bool = False,
        sink: Sink | None = None,
        max_events: int = ...,
        max_content_bytes: int = ...,
    ) -> None: ...
    @property
    def enabled(self) -> bool: ...
    @property
    def traces(self) -> bool: ...
    @property
    def metrics(self) -> bool: ...
    @property
    def events(self) -> bool: ...
    @property
    def level(self) -> TelemetryLevel: ...
    @property
    def sample_rate(self) -> float: ...
    @property
    def include_content(self) -> bool: ...
    @property
    def sink(self) -> Sink | None: ...
    @property
    def max_events(self) -> int: ...
    @property
    def max_content_bytes(self) -> int: ...

def configure(config: TelemetryConfig) -> None: ...
def use(config: TelemetryConfig) -> AbstractContextManager[None]: ...
def flush() -> None: ...
