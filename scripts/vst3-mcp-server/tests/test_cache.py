"""Tests for the read-only tool result cache."""

from __future__ import annotations

from cache import ToolResultCache


def test_read_tools_do_not_invalidate_cache() -> None:
    """no_cache READ tools (get_plugin_state, get_lufs_reading, save_preset) must
    not be classified as writes -- otherwise each such read wipes the entire
    cache, defeating the list_parameters TTL (spec S7.2)."""
    cache = ToolResultCache()
    cache.put("list_parameters", {}, {"status": "success", "parameters": []})

    # Reads must NOT be writes.
    assert cache.is_write_tool("get_plugin_state") is False
    assert cache.is_write_tool("get_lufs_reading") is False
    assert cache.is_write_tool("get_spectrum_snapshot") is False
    assert cache.is_write_tool("list_parameters") is False
    assert cache.is_write_tool("save_preset") is False

    # True writes ARE writes.
    assert cache.is_write_tool("set_output_gain") is True
    assert cache.is_write_tool("set_eq_band") is True
    assert cache.is_write_tool("set_compressor") is True
    assert cache.is_write_tool("apply_mastering_chain") is True
    assert cache.is_write_tool("load_preset") is True
    assert cache.is_write_tool("reset_to_default") is True


def test_write_invalidates_but_read_does_not() -> None:
    cache = ToolResultCache()
    cache.put("list_parameters", {}, {"status": "success", "parameters": [{"id": 1}]})
    assert cache.get("list_parameters", {}) is not None

    # Simulate server.py's dispatch: a write tool invalidates.
    if cache.is_write_tool("set_output_gain"):
        cache.invalidate()
    assert cache.get("list_parameters", {}) is None

    # Re-populate, then a no_cache read must NOT wipe it.
    cache.put("list_parameters", {}, {"status": "success", "parameters": [{"id": 1}]})
    if cache.is_write_tool("get_plugin_state"):
        cache.invalidate()
    assert cache.get("list_parameters", {}) is not None
