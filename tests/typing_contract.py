"""Static consumer examples; checked by mypy or Pyright, not unittest."""

from pathlib import Path
from typing import TYPE_CHECKING, assert_type

from syntwine import MD, Budget, Document, Generated, Source, Text, operation
from syntwine.typing import BuildContext

if TYPE_CHECKING:

    class Settings(Source[dict[str, int]]):
        pass

    @operation(input=Text, output=Text)
    def suffix(value: str, count: int, *, end: str = "!") -> str:
        return value + end * count

    def product_name(context: BuildContext) -> Text:
        return Text(str(context["product"]))

    raw = Source(Path("manifest.json"))
    assert_type(raw, Source[bytes])
    assert_type(raw.as_format(Settings).generate().value, dict[str, int])
    assert_type(Source.defer(product_name), Source[str])

    manifest = raw.as_json().get_text("entry")
    md = manifest.bind(lambda name: Source(name).as_md())
    assert_type(md.as_md(), MD)
    assert_type(manifest.map(len), Source[int])
    assert_type(manifest.pipe(suffix, Source.from_value(2), end=Text(".")), Text)

    doc = (
        Document(format="md")
        .append(md, manifest)
        .pipe(lambda body: b"# Guide\n" + body)
    )
    assert_type(doc.squish().generate(budget=Budget(bytes=500)), Generated[bytes])
