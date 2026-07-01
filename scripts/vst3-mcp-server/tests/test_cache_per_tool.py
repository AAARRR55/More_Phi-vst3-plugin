"""Tests for per-tool cache invalidation (audit fix #9).

``ToolResultCache.invalidate(tool_name)`` must evict ALL entries for that tool,
regardless of their arguments. The prior implementation derived the key prefix
from ``self._key(tool_name, {})[:32]`` -- but the MD5 hexdigest is already 32
chars, so ``[:32]`` yielded the *whole* key for the empty-args entry and the
``startswith`` match silently missed any entry cached with non-empty args.

This is latent today (the server only ever calls ``invalidate()`` with no
argument, doing a full clear), but it's a trap for any caller that needs
targeted invalidation (e.g. the parameter-change invalidation the audit's
cache matrix asks for).
"""

from __future__ import annotations

from cache import ToolResultCache


def test_invalidate_per_tool_evicts_all_arg_variants() -> None:
    cache = ToolResultCache()
    cache.put("list_parameters", {}, {"status": "success", "parameters": []})
    cache.put("list_parameters", {"query": "eq"}, {"status": "success", "parameters": [{"id": 1}]})
    cache.put("list_parameters", {"include_values": True}, {"status": "success", "parameters": []})

    # All three list_parameters entries (different args -> different hashes)
    # must be evicted.
    cache.invalidate("list_parameters")

    assert cache.get("list_parameters", {}) is None
    assert cache.get("list_parameters", {"query": "eq"}) is None
    assert cache.get("list_parameters", {"include_values": True}) is None


def test_invalidate_with_no_argument_clears_everything() -> None:
    # The full-clear path the server actually uses must keep working.
    cache = ToolResultCache()
    cache.put("list_parameters", {}, {"v": 1})

    cache.invalidate()

    assert cache.get("list_parameters", {}) is None


def test_invalidate_unknown_tool_is_noop() -> None:
    cache = ToolResultCache()
    cache.put("list_parameters", {}, {"v": 1})
    cache.invalidate("does_not_exist")  # must not raise, must not clear others
    assert cache.get("list_parameters", {}) is not None
