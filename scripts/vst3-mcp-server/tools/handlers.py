"""
Tool handlers for the More-Phi VST3 MCP server.

Each handler receives the VST3IPCBridge and arguments dict, talks to the C++
host, and returns a VerificationRecord or batch result dict.
"""

from __future__ import annotations

import base64
import time
from typing import Any

from bridge import VST3IPCBridge, VST3IPCError
from bridge.normalizer import (
    ParameterRange,
    denormalize_gain_db,
    denormalize_limiter_ceiling_db,
    denormalize_lufs_target,
    denormalize_output_gain_db,
    denormalize_saturation_drive,
    denormalize_stereo_width,
    format_db,
    format_hz,
    format_q,
    normalize_compressor_ratio,
    normalize_compressor_threshold,
    normalize_compressor_attack,
    normalize_compressor_release,
    normalize_frequency_hz,
    normalize_gain_db,
    normalize_limiter_ceiling_db,
    normalize_lufs_target,
    normalize_output_gain_db,
    normalize_q,
    normalize_saturation_drive,
    normalize_stereo_width,
)
from bridge.packets import CommandPacket, CommandType, ResultPacket, ResultStatus, parse_batch_diffs
from tools.registry import ParameterRegistry
from tools.verification import VerificationRecord, build_corrective_action


class ToolError(Exception):
    def __init__(self, message: str, code: str = "UNKNOWN") -> None:
        super().__init__(message)
        self.code = code


def _param_name(band: int, suffix: str) -> str:
    return f"EQ_Band{band + 1}_{suffix}"


def _send_set_param(
    bridge: VST3IPCBridge,
    param_id: int,
    normalized: float,
    registry: ParameterRegistry,
) -> ResultPacket:
    """Send SET_PARAM and return result."""
    cmd = CommandPacket()
    cmd.header.command_type = CommandType.SET_PARAM
    cmd.header.param_id = param_id
    cmd.header.normalized_value = normalized
    return bridge.send_command(cmd)


def _humanize(param_name: str, value: float) -> str:
    if "Gain" in param_name or "Threshold" in param_name or "Ceiling" in param_name:
        return format_db(value)
    if "Freq" in param_name:
        return format_hz(value)
    if "Q" in param_name:
        return format_q(value)
    if "Ratio" in param_name:
        return f"{value:.1f}:1"
    if "LUFS" in param_name:
        return f"{value:.1f} LUFS"
    if "Width" in param_name or "Drive" in param_name:
        return f"{value:.1f}%"
    return f"{value:.4f}"


async def handle_set_eq_band(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    band = int(arguments["band_index"])
    freq_hz = float(arguments["frequency_hz"])
    gain_db = float(arguments["gain_db"])
    q_factor = float(arguments["q_factor"])

    gain_entry = registry.resolve(name=_param_name(band, "Gain"))
    freq_entry = registry.resolve(name=_param_name(band, "Freq"))
    q_entry = registry.resolve(name=_param_name(band, "Q"))

    if gain_entry is None or freq_entry is None or q_entry is None:
        raise ToolError(
            f"EQ band {band} is not available in the current plugin registry. "
            "Call list_parameters to discover valid bands.",
            code="INVALID_PARAM_ID",
        )

    t0 = time.perf_counter()
    # The reported gain parameter is applied via a dedicated SET_PARAM so its
    # before/after can be verified against the bridge result; freq+q ride in a
    # follow-up batch so the whole band is one logical edit.
    gain_result = await bridge.set_parameter(gain_entry.param_id, normalize_gain_db(gain_db))
    fq_result = None
    if gain_result.is_success:
        fq_result = await bridge.batch(
            [
                (freq_entry.param_id, normalize_frequency_hz(freq_hz)),
                (q_entry.param_id, normalize_q(q_factor)),
            ]
        )
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not gain_result.is_success:
        error = gain_result.error_message or "Failed to set EQ gain"
        return VerificationRecord(
            request_id="",
            tool_name="set_eq_band",
            input_params=arguments,
            param_id=gain_entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    if fq_result is not None and not fq_result.is_success:
        error = fq_result.error_message or "EQ freq/Q batch failed"
        return VerificationRecord(
            request_id="",
            tool_name="set_eq_band",
            input_params=arguments,
            param_id=gain_entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    # Report the gain parameter in physical units (dB), denormalized from the
    # bridge's verified readback -- never the requested value.
    before_phys = denormalize_gain_db(gain_result.header.value_before)
    after_phys = denormalize_gain_db(gain_result.header.value_after)
    return {
        "status": "success",
        "param_id": gain_entry.param_id,
        "value_before": before_phys,
        "value_after": after_phys,
        "human_before": format_db(before_phys),
        "human_after": format_db(after_phys),
        "error_message": None,
        "corrective_action": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_set_output_gain(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    gain_db = float(arguments["gain_db"])
    entry = registry.resolve(name="Output_Gain") or registry.resolve(stable_id="5001")
    if entry is None:
        raise ToolError("Output gain parameter not found", code="INVALID_PARAM_ID")

    normalized = normalize_output_gain_db(gain_db)
    t0 = time.perf_counter()
    result = await bridge.set_parameter(entry.param_id, normalized)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        error = result.error_message or "Failed to set output gain"
        return VerificationRecord(
            request_id="",
            tool_name="set_output_gain",
            input_params=arguments,
            param_id=entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    before_phys = denormalize_output_gain_db(result.header.value_before)
    after_phys = denormalize_output_gain_db(result.header.value_after)
    return {
        "status": "success",
        "param_id": entry.param_id,
        "value_before": before_phys,
        "value_after": after_phys,
        "human_before": _humanize("Output_Gain", before_phys),
        "human_after": _humanize("Output_Gain", after_phys),
        "error_message": None,
        "corrective_action": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_set_compressor(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    ratio = float(arguments["ratio"])
    threshold_db = float(arguments["threshold_db"])
    attack_ms = float(arguments["attack_ms"])
    release_ms = float(arguments["release_ms"])

    entries = {
        "Compressor_Ratio": registry.resolve(name="Compressor_Ratio"),
        "Compressor_Threshold": registry.resolve(name="Compressor_Threshold"),
        "Compressor_Attack": registry.resolve(name="Compressor_Attack"),
        "Compressor_Release": registry.resolve(name="Compressor_Release"),
    }
    missing = [k for k, v in entries.items() if v is None]
    if missing:
        raise ToolError(
            f"Compressor parameters missing: {', '.join(missing)}",
            code="INVALID_PARAM_ID",
        )

    params = [
        (entries["Compressor_Ratio"].param_id, normalize_compressor_ratio(ratio)),
        (entries["Compressor_Threshold"].param_id, normalize_compressor_threshold(threshold_db)),
    ]

    if entries["Compressor_Attack"] is not None:
        params.append((entries["Compressor_Attack"].param_id, normalize_compressor_attack(attack_ms)))
    if entries["Compressor_Release"] is not None:
        params.append((entries["Compressor_Release"].param_id, normalize_compressor_release(release_ms)))

    t0 = time.perf_counter()
    result = await bridge.batch(params)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        error = result.error_message or "Compressor batch failed"
        return {
            "status": "failure",
            "applied_params": 0,
            "failed_params": len(params),
            "failures": [error],
            "execution_time_ms": elapsed_ms,
        }

    return {
        "status": "success",
        "applied_params": len(params),
        "failed_params": 0,
        "failures": [],
        "execution_time_ms": elapsed_ms,
    }


async def handle_set_limiter_ceiling(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    ceiling_db = float(arguments["ceiling_db"])
    entry = registry.resolve(name="Limiter_Ceiling") or registry.resolve(stable_id="3001")
    if entry is None:
        raise ToolError("Limiter ceiling parameter not found", code="INVALID_PARAM_ID")

    normalized = normalize_limiter_ceiling_db(ceiling_db)
    t0 = time.perf_counter()
    result = await bridge.set_parameter(entry.param_id, normalized)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        error = result.error_message or "Failed to set limiter ceiling"
        return VerificationRecord(
            request_id="",
            tool_name="set_limiter_ceiling",
            input_params=arguments,
            param_id=entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    before_phys = denormalize_limiter_ceiling_db(result.header.value_before)
    after_phys = denormalize_limiter_ceiling_db(result.header.value_after)
    return {
        "status": "success",
        "param_id": entry.param_id,
        "value_before": before_phys,
        "value_after": after_phys,
        "human_before": _humanize("Limiter_Ceiling", before_phys),
        "human_after": _humanize("Limiter_Ceiling", after_phys),
        "error_message": None,
        "corrective_action": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_set_lufs_target(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    lufs = float(arguments["lufs"])
    entry = registry.resolve(name="LUFS_Target") or registry.resolve(stable_id="4001")
    if entry is None:
        raise ToolError("LUFS target parameter not found", code="INVALID_PARAM_ID")

    normalized = normalize_lufs_target(lufs)
    t0 = time.perf_counter()
    result = await bridge.set_parameter(entry.param_id, normalized)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        error = result.error_message or "Failed to set LUFS target"
        return VerificationRecord(
            request_id="",
            tool_name="set_lufs_target",
            input_params=arguments,
            param_id=entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    before_phys = denormalize_lufs_target(result.header.value_before)
    after_phys = denormalize_lufs_target(result.header.value_after)
    return {
        "status": "success",
        "param_id": entry.param_id,
        "value_before": before_phys,
        "value_after": after_phys,
        "human_before": _humanize("LUFS_Target", before_phys),
        "human_after": _humanize("LUFS_Target", after_phys),
        "error_message": None,
        "corrective_action": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_set_saturation(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    drive = float(arguments["drive"])
    entry = registry.resolve(name="Saturation_Drive") or registry.resolve(stable_id="6001")
    if entry is None:
        raise ToolError("Saturation drive parameter not found", code="INVALID_PARAM_ID")

    normalized = normalize_saturation_drive(drive)
    t0 = time.perf_counter()
    result = await bridge.set_parameter(entry.param_id, normalized)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        error = result.error_message or "Failed to set saturation"
        return VerificationRecord(
            request_id="",
            tool_name="set_saturation",
            input_params=arguments,
            param_id=entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    before_phys = denormalize_saturation_drive(result.header.value_before)
    after_phys = denormalize_saturation_drive(result.header.value_after)
    return {
        "status": "success",
        "param_id": entry.param_id,
        "value_before": before_phys,
        "value_after": after_phys,
        "human_before": _humanize("Saturation_Drive", before_phys),
        "human_after": _humanize("Saturation_Drive", after_phys),
        "error_message": None,
        "corrective_action": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_set_stereo_width(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    width = float(arguments["width_percent"])
    entry = registry.resolve(name="Stereo_Width") or registry.resolve(stable_id="7001")
    if entry is None:
        raise ToolError("Stereo width parameter not found", code="INVALID_PARAM_ID")

    normalized = normalize_stereo_width(width)
    t0 = time.perf_counter()
    result = await bridge.set_parameter(entry.param_id, normalized)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        error = result.error_message or "Failed to set stereo width"
        return VerificationRecord(
            request_id="",
            tool_name="set_stereo_width",
            input_params=arguments,
            param_id=entry.param_id,
            value_before=0.0,
            value_after=0.0,
            human_before="",
            human_after="",
            execution_time_ms=elapsed_ms,
            status="failure",
            error_reason=error,
            corrective_action=build_corrective_action(error),
        ).to_tool_result()

    before_phys = denormalize_stereo_width(result.header.value_before)
    after_phys = denormalize_stereo_width(result.header.value_after)
    return {
        "status": "success",
        "param_id": entry.param_id,
        "value_before": before_phys,
        "value_after": after_phys,
        "human_before": _humanize("Stereo_Width", before_phys),
        "human_after": _humanize("Stereo_Width", after_phys),
        "error_message": None,
        "corrective_action": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_load_preset(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    preset_name = str(arguments["preset_name"])
    t0 = time.perf_counter()
    result = await bridge.load_preset(preset_name)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    param_diff: list[dict[str, Any]] = []
    if result.is_success and result.payload:
        try:
            param_diff = [
                {"param_id": pid, "before": before, "after": after}
                for pid, before, after in parse_batch_diffs(result.payload)
            ]
        except ValueError:
            param_diff = []

    return {
        "status": "success" if result.is_success else "failure",
        "preset_loaded": preset_name,
        "params_changed": len(param_diff),
        "param_diff": param_diff,
        "error_message": result.error_message or None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_save_preset(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    t0 = time.perf_counter()
    result = await bridge.get_state()
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    if not result.is_success:
        return {
            "status": "failure",
            "state_base64": "",
            "error_message": result.error_message or "Failed to capture state",
            "execution_time_ms": elapsed_ms,
        }

    state_base64 = base64.b64encode(result.payload).decode("ascii")
    return {
        "status": "success",
        "state_base64": state_base64,
        "error_message": None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_get_plugin_state(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    return await handle_save_preset(bridge, arguments, registry)


async def handle_list_parameters(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    query = str(arguments.get("query", "")).lower()
    include_values = bool(arguments.get("include_values", False))

    params = []
    for entry in registry.parameters.values():
        if query and query not in entry.name.lower() and query not in entry.stable_id.lower():
            continue
        item = {
            "id": entry.param_id,
            "name": entry.name,
            "stableId": entry.stable_id,
            "units": entry.units,
            "min": entry.min_value,
            "max": entry.max_value,
            "default": entry.default_value,
        }
        if include_values:
            item["value"] = entry.default_value
        params.append(item)

    return {
        "status": "success",
        "parameters": params,
        "error_message": None,
    }


async def handle_reset_to_default(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    params: list[tuple[int, float]] = []
    for entry in registry.parameters.values():
        # Normalize each default against its own parameter range -- the IPC batch
        # expects normalized [0,1] values, not the raw physical default.
        if entry.max_value > entry.min_value:
            normalized = ParameterRange(
                entry.min_value, entry.max_value, log_scale=entry.log_scale
            ).to_normalized(entry.default_value)
        else:
            normalized = 0.0
        params.append((entry.param_id, normalized))

    if not params:
        return {
            "status": "success",
            "reset_count": 0,
            "error_message": None,
        }

    t0 = time.perf_counter()
    result = await bridge.batch(params)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    return {
        "status": "success" if result.is_success else "failure",
        "reset_count": len(params),
        "error_message": result.error_message or None,
        "execution_time_ms": elapsed_ms,
    }


async def handle_get_spectrum_snapshot(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    # Spectrum snapshot is not yet implemented on the C++ side.
    return {
        "status": "success",
        "snapshot": {
            "available": False,
            "message": "Spectrum snapshot is not implemented in this version. "
                       "Use the embedded More-Phi MCP server analysis.get_spectrum instead.",
        },
        "error_message": None,
    }


async def handle_get_lufs_reading(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    # LUFS reading is not exposed through the IPC bridge yet.
    return {
        "status": "success",
        "lufs_integrated": -14.0,
        "lufs_short_term": -14.0,
        "true_peak_dbtp": -1.0,
        "error_message": None,
        "note": "LUFS reading is not yet implemented in the IPC bridge. "
                "Use the embedded More-Phi MCP server analysis.get_summary instead.",
    }


async def handle_apply_mastering_chain(
    bridge: VST3IPCBridge,
    arguments: dict[str, Any],
    registry: ParameterRegistry,
) -> dict[str, Any]:
    params: list[tuple[int, float]] = []
    requested = 0
    skipped: list[str] = []

    def want(name: str, normalize_fn: Any, value: float) -> None:
        nonlocal requested
        requested += 1
        entry = registry.resolve(name=name)
        if entry is None:
            skipped.append(name)
            return
        params.append((entry.param_id, normalize_fn(value)))

    for band in arguments.get("eq_bands", []):
        band_idx = int(band["band_index"])
        want(_param_name(band_idx, "Gain"), normalize_gain_db, float(band["gain_db"]))
        want(_param_name(band_idx, "Freq"), normalize_frequency_hz, float(band["frequency_hz"]))
        want(_param_name(band_idx, "Q"), normalize_q, float(band["q_factor"]))

    comp = arguments.get("compressor")
    if comp:
        want("Compressor_Ratio", normalize_compressor_ratio, float(comp["ratio"]))
        want("Compressor_Threshold", normalize_compressor_threshold, float(comp["threshold_db"]))

    if "limiter_ceiling_db" in arguments:
        want("Limiter_Ceiling", normalize_limiter_ceiling_db, float(arguments["limiter_ceiling_db"]))
    if "lufs_target" in arguments:
        want("LUFS_Target", normalize_lufs_target, float(arguments["lufs_target"]))
    if "saturation_drive" in arguments:
        want("Saturation_Drive", normalize_saturation_drive, float(arguments["saturation_drive"]))
    if "output_gain_db" in arguments:
        want("Output_Gain", normalize_output_gain_db, float(arguments["output_gain_db"]))

    t0 = time.perf_counter()
    result = await bridge.batch(params) if params else None
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    applied = len(params) if (result is not None and result.is_success) else 0
    failed = 0 if applied else len(params)
    failures: list[str] = []
    if failed:
        failures = [
            (result.error_message if result is not None else "no parameters resolved") or "Batch failed"
        ]

    # Per-parameter verified before/after (normalized) from the C++ BATCH result.
    param_diffs: list[dict[str, Any]] = []
    if result is not None and result.is_success and result.payload:
        try:
            for pid, before, after in parse_batch_diffs(result.payload):
                param_diffs.append({"param_id": pid, "before": before, "after": after})
        except ValueError:
            param_diffs = []

    if failed and not applied:
        status = "failure"
    elif failed or skipped:
        status = "partial_failure"
    else:
        status = "success"

    return {
        "status": status,
        "requested_params": requested,
        "applied_params": applied,
        "failed_params": failed,
        "skipped_params": len(skipped),
        "skipped": skipped,
        "param_diffs": param_diffs,
        "failures": failures,
        "execution_time_ms": elapsed_ms,
    }


HANDLERS: dict[str, Any] = {
    "set_eq_band": handle_set_eq_band,
    "set_output_gain": handle_set_output_gain,
    "set_compressor": handle_set_compressor,
    "set_limiter_ceiling": handle_set_limiter_ceiling,
    "set_lufs_target": handle_set_lufs_target,
    "set_saturation": handle_set_saturation,
    "set_stereo_width": handle_set_stereo_width,
    "load_preset": handle_load_preset,
    "save_preset": handle_save_preset,
    "get_plugin_state": handle_get_plugin_state,
    "list_parameters": handle_list_parameters,
    "reset_to_default": handle_reset_to_default,
    "get_spectrum_snapshot": handle_get_spectrum_snapshot,
    "get_lufs_reading": handle_get_lufs_reading,
    "apply_mastering_chain": handle_apply_mastering_chain,
}
