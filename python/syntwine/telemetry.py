"""Configure native build traces and explicitly flush their configured sinks."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from typing import Literal

from ._syntwine_native import _telemetry as _native

TelemetryConfig = _native.TelemetryConfig
Sink = _native.Sink
MemorySink = _native.MemorySink
JSONLSink = _native.JSONLSink
SQLiteSink = _native.SQLiteSink
OTLPSink = _native.OTLPSink

Trace = dict[str, object]
SpanRecord = dict[str, object]
Dependency = dict[str, object]
type TelemetryLevel = Literal["build", "stages", "operations"]


def configure(config: TelemetryConfig) -> None:
    """Set the process's default configuration for subsequent builds."""
    _native.configure(config)


@contextmanager
def use(config: TelemetryConfig) -> Iterator[None]:
    """Override the default for the current context, including its async tasks."""
    token = _native._push_config(config)
    try:
        yield
    finally:
        _native._reset_config(token)


def flush() -> None:
    """Export pending records from all active sinks; raise on an export failure."""
    _native.flush()


__all__ = [
    "Dependency",
    "JSONLSink",
    "MemorySink",
    "OTLPSink",
    "SQLiteSink",
    "Sink",
    "SpanRecord",
    "TelemetryConfig",
    "TelemetryLevel",
    "Trace",
    "configure",
    "flush",
    "use",
]
