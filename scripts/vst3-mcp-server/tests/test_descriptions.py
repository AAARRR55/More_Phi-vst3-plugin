"""Tests for tool description/schema documentation completeness (audit fixes
#7, #10).

  * #7: every tool description must carry at least one example input so the
    model has a concrete shape to imitate.
  * #10: load_preset.preset_name (no list_presets tool exists to source it from)
    must at least bound the string length so a model can't send a megabyte of
    hallucinated name.
"""

from __future__ import annotations

import re

from tools import get_tool_descriptions


_EXAMPLE_PATTERNS = [
    re.compile(r"example", re.IGNORECASE),
    re.compile(r"e\.g\.", re.IGNORECASE),
    # A JSON-ish snippet like {"gain_db": -3.0} counts as an example.
    re.compile(r'\{"[^"]+"\s*:'),
]


def _has_example(description: str) -> bool:
    return any(p.search(description) for p in _EXAMPLE_PATTERNS)


def test_every_tool_description_has_an_example() -> None:
    missing = [
        t["name"] for t in get_tool_descriptions()
        if not _has_example(t["description"])
    ]
    assert not missing, f"tools missing examples in description: {missing}"


def test_every_tool_description_names_its_inputs() -> None:
    # Descriptions should reference the parameter names so the model learns the
    # contract from prose, not only from the schema.
    for t in get_tool_descriptions():
        props = t["inputSchema"].get("properties", {})
        if not props:
            continue  # no-arg tools are exempt
        # At least one parameter name should appear in the description.
        assert any(p in t["description"] for p in props), (
            f"{t['name']}: description doesn't mention any of its parameters {list(props)}"
        )


def test_load_preset_name_is_length_bounded() -> None:
    tool = next(t for t in get_tool_descriptions() if t["name"] == "load_preset")
    name_prop = tool["inputSchema"]["properties"]["preset_name"]
    # No list_presets tool exists to source valid names from, so at minimum the
    # string must be length-bounded to prevent abuse / hallucination floods.
    assert "maxLength" in name_prop, "preset_name must declare maxLength"
