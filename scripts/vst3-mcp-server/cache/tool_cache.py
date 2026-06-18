"""
LRU + TTL result cache for read-only MCP tools.
"""

from __future__ import annotations

import hashlib
import json
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Literal


CacheStrategy = Literal["ttl", "no_cache", "stale_revalidate"]


TOOL_CACHE_POLICY: dict[str, dict[str, Any]] = {
    "list_parameters": {"ttl_s": 300, "strategy": "ttl"},
    "get_plugin_state": {"ttl_s": 0, "strategy": "no_cache"},
    "get_spectrum_snapshot": {"ttl_s": 0.1, "strategy": "stale_revalidate"},
    "get_lufs_reading": {"ttl_s": 0, "strategy": "no_cache"},
    "save_preset": {"ttl_s": 0, "strategy": "no_cache"},
}


@dataclass
class _CacheEntry:
    value: Any
    cached_at: float = field(default_factory=time.perf_counter)
    stale_at: float | None = None


class ToolResultCache:
    """Thread-safe is not required; the server runs on a single asyncio loop."""

    def __init__(self, max_size: int = 128) -> None:
        self._max_size = max_size
        self._store: OrderedDict[str, _CacheEntry] = OrderedDict()

    @staticmethod
    def _key(tool_name: str, arguments: dict[str, Any]) -> str:
        canonical = json.dumps(arguments, sort_keys=True, separators=(",", ":"))
        return hashlib.md5(f"{tool_name}:{canonical}".encode("utf-8")).hexdigest()

    def get(self, tool_name: str, arguments: dict[str, Any]) -> Any | None:
        policy = TOOL_CACHE_POLICY.get(tool_name, {"ttl_s": 0, "strategy": "no_cache"})
        if policy["strategy"] == "no_cache":
            return None

        key = self._key(tool_name, arguments)
        entry = self._store.get(key)
        if entry is None:
            return None

        now = time.perf_counter()
        if policy["strategy"] == "ttl" and now - entry.cached_at > policy["ttl_s"]:
            self._store.pop(key, None)
            return None

        # stale_revalidate: return stale entry; caller can refresh in background.
        self._store.move_to_end(key)
        return entry.value

    def put(self, tool_name: str, arguments: dict[str, Any], value: Any) -> None:
        policy = TOOL_CACHE_POLICY.get(tool_name, {"ttl_s": 0, "strategy": "no_cache"})
        if policy["strategy"] == "no_cache":
            return

        key = self._key(tool_name, arguments)
        self._store[key] = _CacheEntry(value=value)
        self._store.move_to_end(key)

        while len(self._store) > self._max_size:
            self._store.popitem(last=False)

    def invalidate(self, tool_name: str | None = None) -> None:
        if tool_name is None:
            self._store.clear()
            return
        keys = [k for k in self._store if k.startswith(self._key(tool_name, {})[:32])]
        for k in keys:
            self._store.pop(k, None)

    def is_write_tool(self, tool_name: str) -> bool:
        return tool_name in _write_tools()


_WRITE_TOOLS: frozenset[str] | None = None


def _write_tools() -> frozenset[str]:
    """Write tools derived from the registry annotations (readOnlyHint is False).

    Lazy import avoids any module-load cycle with tools.registry. Classifying by
    annotation -- not by cache strategy -- keeps no_cache READ tools
    (get_plugin_state, get_lufs_reading, save_preset) from wiping the cache.
    """
    global _WRITE_TOOLS
    if _WRITE_TOOLS is None:
        from tools.registry import get_tool_descriptions

        _WRITE_TOOLS = frozenset(
            descriptor["name"]
            for descriptor in get_tool_descriptions()
            if descriptor.get("annotations", {}).get("readOnlyHint") is False
        )
    return _WRITE_TOOLS
