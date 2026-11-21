from __future__ import annotations

import json
import tomllib
import xml.etree.ElementTree as ET
from unittest import TestCase

from syntwine import (
    JSON,
    MD,
    TOML,
    XML,
    DecodeError,
    Document,
    EncodingError,
    Literal,
    ParseError,
    Text,
)


class FormatTests(TestCase):
    def test_json_selection_merge_and_reinterpretation(self) -> None:
        payload = JSON('{"enabled":false, "meta":{"old":1}, "items":[{"name":"A"}]}')
        overrides = JSON.from_value(
            {"meta": {"new": 2}, "items": [{"name": "B"}, {"name": "C"}]}
        )
        merged = payload.merge(overrides, conflict="right").set("enabled", True)
        selected = merged.select("$.items[*].name")
        self.assertIsInstance(selected, JSON)
        self.assertEqual(selected.generate().value, ["B", "C"])
        self.assertEqual(merged.get("meta").generate().value, {"old": 1, "new": 2})
        self.assertEqual(merged.get("enabled").generate().value, True)
        self.assertEqual(
            merged.remove("items").generate().value,
            {"enabled": True, "meta": {"old": 1, "new": 2}},
        )
        self.assertEqual(payload.generate().value["items"], [{"name": "A"}])
        pretty = merged.pretty(indent=2).generate().text
        self.assertIn('\n  "enabled"', pretty)
        self.assertEqual(
            json.loads(pretty), json.loads(merged.minify().generate().text)
        )
        with self.assertRaises(ParseError):
            TOML("enabled = true").as_json().generate()
        with self.assertRaises(ParseError) as error:
            Document().append(Text("prefix"), Literal(b'{"x":]').as_json()).generate()
        self.assertGreaterEqual(len(error.exception.dependency_path), 2)

    def test_toml_table_view_keeps_comments_and_can_convert_semantics(self) -> None:
        original = (
            "# launch configuration\n"
            "[agent]\n"
            "mode = 'local'  # keep my note\n"
            "temp = true\n"
            "\"réglage\"    = '''first\nold'''  # keep multiline note\n"
            "\n"
            "[ui]\n"
            'theme = "moon"\n'
        ).replace("\n", "\r\n")
        embedded = Document().append(Text("new"), Text("multi\nline")).as_literal()
        inputs = TOML(original)
        edits = (
            inputs.table("agent")
            .set("mode", Text("remote"))
            .set("réglage", embedded)
            .remove("temp")
        )
        final = edits.root().generate()
        parsed = tomllib.loads(final.text)
        agent = {"mode": "remote", "réglage": "new\n\nmulti\nline"}
        self.assertEqual(parsed, {"agent": agent, "ui": {"theme": "moon"}})
        for untouched in (
            "# launch configuration",
            "# keep my note",
            "# keep multiline note\r\n",
            '"réglage"    = ',
            '[ui]\r\ntheme = "moon"',
        ):
            self.assertIn(untouched, final.text)
        self.assertEqual(inputs.generate().text, original)
        self.assertEqual(edits.to_json().generate().value, agent)
        self.assertEqual(JSON.from_value(parsed).to_toml().generate().value, parsed)

    def test_binary_argument_requires_strict_utf8_before_toml_serialization(
        self,
    ) -> None:
        doc = Document(format="raw").append(Literal(b"\xff\x00"))
        invalid = TOML.from_value({}).set("payload", doc.as_literal())
        with self.assertRaises(EncodingError):
            invalid.generate()
        valid = TOML.from_value({}).set("base64", doc.as_literal().base64())
        self.assertEqual(tomllib.loads(valid.generate().text), {"base64": "/wA="})
        with self.assertRaises(DecodeError):
            doc.as_text().generate()

    def test_markdown_structure_preserves_fences_and_section_boundaries(self) -> None:
        code = "```markdown\n# Not a heading\n\n    indented  \n```"
        markdown = MD(
            f"# Agent\n\n{code}\n\n## Work\n\n[A link](https://example.org)\n"
            "\n### Subtask\n\n- Keep\n\n## Appendix\n\nNothing"
        )
        selected = markdown.section("Work").shift_headings(1).render().generate().text
        self.assertTrue(selected.startswith("### Work"))
        self.assertIn("#### Subtask", selected)
        self.assertNotIn("Appendix", selected)
        self.assertNotIn("Not a heading", str(markdown.headings().generate().value))
        self.assertIn("https://example.org", str(markdown.links().generate().value))
        compact = Document(format="md").append(markdown).squish().generate().text
        self.assertIn(code, compact)
        self.assertIn("- Keep", compact)
        self.assertIn("[A link](https://example.org)", compact)
        setext = MD("Setext\r\n======\r\nfollowing\r\n")
        self.assertEqual(
            setext.shift_headings(2).generate().text, "### Setext\r\nfollowing\r\n"
        )

    def test_xml_xpath_edits_and_explicit_escaping(self) -> None:
        value = Text("a < b & more")
        note = XML.element("note", value)
        self.assertEqual(ET.fromstring(note.generate().text).text, "a < b & more")
        raw = XML(
            "<root><instruction>old</instruction><instruction>stay</instruction></root>"
        )
        selected = (
            raw.xpath("//instruction")
            .set_attr("priority", Text("high"))
            .set_text(value)
            .wrap("instructions")
        )
        fragment = ET.fromstring("<result>" + selected.generate().text + "</result>")
        children = fragment.findall(".//instructions/instruction")
        self.assertEqual(len(children), 2)
        self.assertTrue(all(child.get("priority") == "high" for child in children))
        self.assertTrue(all(child.text == "a < b & more" for child in children))
        self.assertEqual(ET.fromstring(raw.generate().text)[0].text, "old")

        namespaced = XML.element("root", XML.element("item"), attrs={"xmlns": "urn:a"})
        parsed = ET.fromstring(namespaced.generate().text)
        self.assertEqual((parsed.tag, parsed[0].tag), ("{urn:a}root", "item"))

    def test_xml_squish_preserves_mixed_text_and_xml_space(self) -> None:
        mixed = "<p>one <b> two </b> three</p>"
        preserved = '<code xml:space="preserve"> <b>x</b> </code>'
        source = f"<root>\n  {mixed}\n  {preserved}\n</root>"
        squeezed = Document(format="xml").append(Text(source)).squish().generate().text
        self.assertIn(mixed, squeezed)
        self.assertIn(preserved, squeezed)
        self.assertEqual(ET.fromstring(squeezed).tag, "root")
        for malformed in ("<a>", "<a><b></a>"):
            with self.assertRaises(ParseError):
                Text(malformed).as_xml().generate()

    def test_text_format_and_newlines_use_evaluated_sources(self) -> None:
        recipe = (
            Text("  Hello, {who}!  ")
            .format(who=Text("世界"))
            .strip()
            .replace("Hello", "Welcome")
            .indent(2)
        )
        joined = (
            Text("|")
            .join([recipe, Text("end\r\n")])
            .normalize_newlines("lf")
            .generate()
        )
        self.assertEqual(joined.text, "  Welcome, 世界!|end\n")
        self.assertEqual(joined.bytes, joined.text.encode("utf-8"))
