# Budgets and diagnostics

Set budgets on the terminal build. The checks apply to the final serialized
output, including separators, escaping, and post-processing. Use bytes for
binary output, characters for Unicode code points, and tokens with the
encoding required by the consumer. A tokenizer can also be supplied without a
budget if only a count is needed.

```python
from syntwine import Budget, BudgetExceeded, Document, Text

doc = Document().append(Text("Hello, Klee!"))
artifact = doc.generate(budget=Budget(tokens=5), tokenizer="o200k_base")
assert artifact.usage.tokens == 5

try:
    doc.generate(budget=Budget(tokens=4), tokenizer="o200k_base")
except BudgetExceeded as error:
    assert (error.dimension, error.actual, error.budget) == ("tokens", 5, 4)
else:
    raise AssertionError("the token limit must reject this output")
```

When a budget fails, report the measured size and limit. Choose what to
remove or summarize based on task priorities; do not silently truncate or
change the budget. `.write()` validates before replacing the destination.

## Find where a build failed

1. On a `SyntwineError`, inspect `source_id`, `location`, `operation`,
   `dependency_path`, and `cause` where present. These distinguish read,
   decode, parse, callback, and serialization failures.
2. Use `source.inspect()` to inspect the declared graph without reading
   files or running callbacks.
3. When the failing dependency is selected at runtime or timings matter,
   enable telemetry for the individual build. A sink is not required for
   `artifact.trace` or `error.trace`. Use persistent or remote sinks only
   when the requested workflow needs an export.

```python
from syntwine import Text
from syntwine.telemetry import TelemetryConfig

result = Text("done").generate(telemetry=TelemetryConfig(enabled=True))
assert result.trace is not None and result.trace["status"] == "ok"
```
