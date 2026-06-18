"""
Tool registry and parameter mapping for the More-Phi VST3 MCP server.

Tools are defined with JSON Schema 2020-12 input/output schemas and behavioral
annotations per the MCP 2025-06-18 specification.
"""

from __future__ import annotations

import json
import os
import platform
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class ParameterEntry:
    param_id: int
    name: str
    stable_id: str
    units: str
    min_value: float
    max_value: float
    default_value: float
    log_scale: bool


class ParameterRegistry:
    """Loads and resolves parameters from the C++ host's exported registry."""

    def __init__(self, instance_id: str | None = None) -> None:
        self.instance_id = instance_id or "default"
        self.parameters: dict[int, ParameterEntry] = {}
        self.by_name: dict[str, ParameterEntry] = {}
        self.by_stable_id: dict[str, ParameterEntry] = {}
        self.reload()

    @classmethod
    def default_path(cls, instance_id: str) -> Path:
        temp = Path(tempfile.gettempdir())
        return temp / f"more_phi_vst3_mcp_{instance_id}_registry.json"

    def reload(self) -> None:
        path = self.default_path(self.instance_id)
        fallback = Path(__file__).parent.parent / "schema" / "param_registry.json"

        data: dict[str, Any] = {}
        if path.exists():
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
            except Exception:
                data = {}

        if not data and fallback.exists():
            try:
                with open(fallback, "r", encoding="utf-8") as f:
                    data = json.load(f)
            except Exception:
                data = {}

        self.parameters.clear()
        self.by_name.clear()
        self.by_stable_id.clear()

        for entry in data.get("parameters", []):
            param_id = int(entry.get("id", entry.get("index", -1)))
            if param_id < 0:
                continue
            stable_id = str(entry.get("stableId", param_id))
            param = ParameterEntry(
                param_id=param_id,
                name=str(entry.get("name", f"param_{param_id}")),
                stable_id=stable_id,
                units=str(entry.get("units", "")),
                min_value=float(entry.get("min", 0.0)),
                max_value=float(entry.get("max", 1.0)),
                default_value=float(entry.get("default", 0.0)),
                log_scale=bool(entry.get("log", False)),
            )
            self.parameters[param_id] = param
            self.by_name[param.name] = param
            self.by_stable_id[stable_id] = param

    def resolve(
        self,
        name: str | None = None,
        stable_id: str | None = None,
        param_id: int | None = None,
    ) -> ParameterEntry | None:
        if param_id is not None and param_id in self.parameters:
            return self.parameters[param_id]
        if stable_id is not None and stable_id in self.by_stable_id:
            return self.by_stable_id[stable_id]
        if name is not None and name in self.by_name:
            return self.by_name[name]
        return None


# ---------------------------------------------------------------------------
# Tool schemas
# ---------------------------------------------------------------------------

def _tool(
    name: str,
    description: str,
    input_schema: dict[str, Any],
    output_schema: dict[str, Any],
    annotations: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "name": name,
        "description": description,
        "inputSchema": input_schema,
        "outputSchema": output_schema,
        "annotations": annotations or {},
    }


_ANNOTATION_WRITE = {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": True}
_ANNOTATION_WRITE_DESTRUCTIVE = {
    "readOnlyHint": False,
    "destructiveHint": True,
    "idempotentHint": False,
}
_ANNOTATION_READ = {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True}


EQ_BAND_INPUT = {
    "type": "object",
    "properties": {
        "band_index": {"type": "integer", "minimum": 0, "maximum": 7},
        "frequency_hz": {"type": "number", "minimum": 20.0, "maximum": 20000.0},
        "gain_db": {"type": "number", "minimum": -24.0, "maximum": 24.0},
        "q_factor": {"type": "number", "minimum": 0.1, "maximum": 10.0},
    },
    "required": ["band_index", "frequency_hz", "gain_db", "q_factor"],
}


SINGLE_PARAM_OUTPUT = {
    "type": "object",
    "properties": {
        "status": {"type": "string", "enum": ["success", "failure"]},
        "param_id": {"type": "integer"},
        "value_before": {"type": "number"},
        "value_after": {"type": "number"},
        "human_before": {"type": "string"},
        "human_after": {"type": "string"},
        "error_message": {"type": ["string", "null"]},
        "corrective_action": {"type": ["string", "null"]},
    },
    "required": ["status"],
}


def get_tool_descriptions() -> list[dict[str, Any]]:
    """Return the complete list of tool descriptors."""
    return [
        _tool(
            "set_eq_band",
            "Set a parametric EQ band (freq, gain, Q) on the mastering chain.",
            EQ_BAND_INPUT,
            SINGLE_PARAM_OUTPUT,
            _ANNOTATION_WRITE,
        ),
        _tool(
            "set_output_gain",
            "Set the output gain in dB.",
            {
                "type": "object",
                "properties": {
                    "gain_db": {"type": "number", "minimum": -24.0, "maximum": 24.0}
                },
                "required": ["gain_db"],
            },
            SINGLE_PARAM_OUTPUT,
            _ANNOTATION_WRITE,
        ),
        _tool(
            "set_compressor",
            "Set the compressor ratio, threshold, attack and release.",
            {
                "type": "object",
                "properties": {
                    "ratio": {"type": "number", "minimum": 1.0, "maximum": 20.0},
                    "threshold_db": {"type": "number", "minimum": -60.0, "maximum": 0.0},
                    "attack_ms": {"type": "number", "minimum": 0.1, "maximum": 100.0},
                    "release_ms": {"type": "number", "minimum": 10.0, "maximum": 1000.0},
                },
                "required": ["ratio", "threshold_db", "attack_ms", "release_ms"],
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure", "partial_failure"]},
                    "applied_params": {"type": "integer"},
                    "failed_params": {"type": "integer"},
                    "failures": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["status"],
            },
            _ANNOTATION_WRITE,
        ),
        _tool(
            "set_limiter_ceiling",
            "Set the true-peak limiter ceiling in dBTP.",
            {
                "type": "object",
                "properties": {
                    "ceiling_db": {"type": "number", "minimum": -12.0, "maximum": 0.0}
                },
                "required": ["ceiling_db"],
            },
            SINGLE_PARAM_OUTPUT,
            _ANNOTATION_WRITE,
        ),
        _tool(
            "set_lufs_target",
            "Set the integrated loudness target in LUFS.",
            {
                "type": "object",
                "properties": {
                    "lufs": {"type": "number", "minimum": -23.0, "maximum": -6.0}
                },
                "required": ["lufs"],
            },
            SINGLE_PARAM_OUTPUT,
            _ANNOTATION_WRITE,
        ),
        _tool(
            "set_saturation",
            "Set the saturation drive amount in percent.",
            {
                "type": "object",
                "properties": {
                    "drive": {"type": "number", "minimum": 0.0, "maximum": 100.0},
                    "tone": {"type": "number", "minimum": 0.0, "maximum": 100.0, "default": 50.0},
                    "mode": {"type": "string", "enum": ["tape", "tube", "transistor"], "default": "tube"},
                },
                "required": ["drive"],
            },
            SINGLE_PARAM_OUTPUT,
            _ANNOTATION_WRITE,
        ),
        _tool(
            "set_stereo_width",
            "Set the mid/side stereo width in percent.",
            {
                "type": "object",
                "properties": {
                    "width_percent": {"type": "number", "minimum": 0.0, "maximum": 200.0}
                },
                "required": ["width_percent"],
            },
            SINGLE_PARAM_OUTPUT,
            _ANNOTATION_WRITE,
        ),
        _tool(
            "load_preset",
            "Load a named preset into the hosted plugin.",
            {
                "type": "object",
                "properties": {"preset_name": {"type": "string"}},
                "required": ["preset_name"],
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "preset_loaded": {"type": "string"},
                    "params_changed": {"type": "integer"},
                    "param_diff": {"type": "array"},
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_WRITE_DESTRUCTIVE,
        ),
        _tool(
            "save_preset",
            "Capture the current hosted plugin state as a Base64-encoded preset blob.",
            {
                "type": "object",
                "properties": {},
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "state_base64": {"type": "string"},
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_READ,
        ),
        _tool(
            "get_plugin_state",
            "Return the current hosted plugin state as a Base64-encoded blob.",
            {
                "type": "object",
                "properties": {},
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "state_base64": {"type": "string"},
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_READ,
        ),
        _tool(
            "list_parameters",
            "Enumerate all hosted plugin parameters with IDs and ranges.",
            {
                "type": "object",
                "properties": {
                    "query": {"type": "string"},
                    "include_values": {"type": "boolean", "default": False},
                },
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "parameters": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "id": {"type": "integer"},
                                "name": {"type": "string"},
                                "stableId": {"type": "string"},
                                "units": {"type": "string"},
                                "min": {"type": "number"},
                                "max": {"type": "number"},
                                "default": {"type": "number"},
                            },
                        },
                    },
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_READ,
        ),
        _tool(
            "reset_to_default",
            "Reset all hosted plugin parameters to their default values.",
            {
                "type": "object",
                "properties": {},
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "reset_count": {"type": "integer"},
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_WRITE_DESTRUCTIVE,
        ),
        _tool(
            "get_spectrum_snapshot",
            "Return a recent spectrum analysis snapshot.",
            {
                "type": "object",
                "properties": {},
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "snapshot": {"type": "object"},
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_READ,
        ),
        _tool(
            "get_lufs_reading",
            "Return the current integrated LUFS reading.",
            {
                "type": "object",
                "properties": {},
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "failure"]},
                    "lufs_integrated": {"type": "number"},
                    "lufs_short_term": {"type": "number"},
                    "true_peak_dbtp": {"type": "number"},
                    "error_message": {"type": ["string", "null"]},
                },
                "required": ["status"],
            },
            _ANNOTATION_READ,
        ),
        _tool(
            "apply_mastering_chain",
            "Apply a complete mastering chain configuration atomically.",
            {
                "type": "object",
                "properties": {
                    "eq_bands": {
                        "type": "array",
                        "items": EQ_BAND_INPUT,
                    },
                    "compressor": {
                        "type": "object",
                        "properties": {
                            "ratio": {"type": "number", "minimum": 1.0, "maximum": 20.0},
                            "threshold_db": {"type": "number", "minimum": -60.0, "maximum": 0.0},
                            "attack_ms": {"type": "number", "minimum": 0.1, "maximum": 100.0},
                            "release_ms": {"type": "number", "minimum": 10.0, "maximum": 1000.0},
                        },
                    },
                    "limiter_ceiling_db": {"type": "number", "minimum": -12.0, "maximum": 0.0},
                    "lufs_target": {"type": "number", "minimum": -23.0, "maximum": -6.0},
                    "saturation_drive": {"type": "number", "minimum": 0.0, "maximum": 100.0},
                    "output_gain_db": {"type": "number", "minimum": -24.0, "maximum": 24.0},
                },
            },
            {
                "type": "object",
                "properties": {
                    "status": {"type": "string", "enum": ["success", "partial_failure", "failure"]},
                    "requested_params": {"type": "integer"},
                    "applied_params": {"type": "integer"},
                    "failed_params": {"type": "integer"},
                    "skipped_params": {"type": "integer"},
                    "skipped": {"type": "array", "items": {"type": "string"}},
                    "param_diffs": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "param_id": {"type": "integer"},
                                "before": {"type": "number"},
                                "after": {"type": "number"},
                            },
                        },
                    },
                    "failures": {"type": "array", "items": {"type": "string"}},
                    "execution_time_ms": {"type": "number"},
                },
                "required": ["status"],
            },
            _ANNOTATION_WRITE,
        ),
    ]
