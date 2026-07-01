"""Input validation for MCP tool calls (audit fixes #1, #2, #3, #5).

Validates tool arguments against the declared inputSchema BEFORE dispatch, so:

  * #1 structural errors (wrong type, missing required) are caught and reported
    as structured failures instead of leaking as generic "Internal error";
  * #2 out-of-range values are REJECTED (not silently clamped by the normalizer);
  * #3 the error carries the full structured contract (code / parameter /
    received / valid_range / suggestion) the LLM can branch on;
  * #5 the catch-all ``except Exception`` in ``server.call_tool`` stops masking
    client input errors as host-log problems.

The validator is fail-closed: a schema lookup miss (unknown tool) returns None
and lets the caller's UnknownTool branch handle it. A known tool whose schema
does not parse is also None (defensive) so the handler still runs.
"""

from __future__ import annotations

from collections import deque
from typing import Any

import jsonschema
from jsonschema import Draft202012Validator

from tools.registry import get_tool_descriptions
from tools.verification import CORRECTIVE_ACTIONS, build_corrective_action


class ValidationError(Exception):
    """Raised when a tool's arguments violate its declared inputSchema.

    Carries the structured fields the audit requires so the server can emit a
    machine-actionable error dict instead of a bare string. Subclassing
    ToolError would create an import cycle (verification imports nothing here,
    but handlers import verification), so this stays self-contained and the
    server maps both ToolError and ValidationError onto the same shape.
    """

    def __init__(
        self,
        message: str,
        *,
        code: str,
        parameter: str | None = None,
        received: Any = None,
        valid_range: Any = None,
        suggestion: str | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.parameter = parameter
        self.received = received
        self.valid_range = valid_range
        self.suggestion = suggestion

    def to_error_dict(self) -> dict[str, Any]:
        """Build the wire error dict. Enriches the legacy status/error_message/
        corrective_action shape with the structured fields, so existing output
        schemas (which allow additional properties) keep validating."""
        corrective = self.suggestion or build_corrective_action(
            self.code, {"min": _bound(self.valid_range, 0), "max": _bound(self.valid_range, 1)}
        )
        return {
            # Structured contract (audit fix #3).
            "error": True,
            "code": self.code,
            "parameter": self.parameter,
            "received": self.received,
            "valid_range": self.valid_range,
            "suggestion": corrective if self.suggestion is None else self.suggestion,
            # Legacy fields (preserved so existing tests/clients keep working).
            "status": "failure",
            "error_message": str(self),
            "corrective_action": corrective,
        }


def _bound(valid_range: Any, index: int) -> Any:
    """Best-effort numeric bound for corrective-action interpolation. The
    CORRECTIVE_ACTIONS PARAM_OUT_RANGE template references {min}/{max}; if the
    schema gave us numeric bounds, surface them so the LLM gets real numbers
    instead of literal '{min}' placeholders."""
    if isinstance(valid_range, (list, tuple)) and len(valid_range) > index:
        return valid_range[index]
    return ""


# Schema cache. Schemas are static (built once in registry at import), so the
# validator instances are reused across requests. Keyed by tool name.
_VALIDATORS: dict[str, Draft202012Validator] | None = None


def _validators() -> dict[str, Draft202012Validator]:
    global _VALIDATORS
    if _VALIDATORS is None:
        _VALIDATORS = {
            descriptor["name"]: Draft202012Validator(descriptor["inputSchema"])
            for descriptor in get_tool_descriptions()
        }
    return _VALIDATORS


def _schema_for(tool_name: str) -> dict[str, Any] | None:
    """Return the inputSchema for a tool by name (linear scan; tool count is 15)."""
    for descriptor in get_tool_descriptions():
        if descriptor["name"] == tool_name:
            return descriptor["inputSchema"]
    return None


def _leaf_schema(root: dict[str, Any], path: deque) -> dict[str, Any] | None:
    """Walk the schema along the absolute path of the failing element and return
    the sub-schema at that point (used to recover valid_range from the schema
    for minimum/maximum/enum violations on nested properties)."""
    node: Any = root
    for segment in path:
        if not isinstance(node, dict):
            return None
        if isinstance(segment, int):
            items = node.get("items")
            node = items if isinstance(items, dict) else None
        else:
            props = node.get("properties", {})
            node = props.get(segment)
        if node is None:
            return None
    return node if isinstance(node, dict) else None


def validate_arguments(tool_name: str, arguments: dict[str, Any]) -> None:
    """Validate ``arguments`` against the tool's declared inputSchema.

    Returns None on success. Raises ValidationError on the first violation with
    the structured contract attached. Unknown tools return None (caller handles
    them via its own UnknownTool branch); this function only fails-closed on
    schema *violations*, not on lookup misses.
    """
    schema = _schema_for(tool_name)
    if schema is None:
        return None

    validator = _validators().get(tool_name)
    if validator is None:
        return None

    error = next(iter(validator.iter_errors(arguments)), None)
    if error is None:
        return None

    raise _translate(tool_name, schema, error)


def _translate(
    tool_name: str, schema: dict[str, Any], error: jsonschema.ValidationError
) -> ValidationError:
    """Convert a jsonschema.ValidationError into a structured ValidationError.

    jsonschema's error model exposes ``validator`` (which keyword failed),
    ``validator_value`` (the constraint), ``path`` (absolute path to the
    offending element), and ``message`` (human text). We map these onto our
    code/parameter/received/valid_range contract.
    """
    path_segments = [seg for seg in error.absolute_path]
    # The parameter name: the last named segment, or the whole path joined.
    parameter = ".".join(str(seg) for seg in path_segments) if path_segments else "(root)"
    leaf_parameter = next(
        (str(seg) for seg in reversed(path_segments) if isinstance(seg, str)),
        parameter,
    )
    received = error.instance if hasattr(error, "instance") else None
    leaf_schema = _leaf_schema(schema, deque(path_segments)) or {}

    validator = error.validator
    validator_value = error.validator_value

    if validator == "required":
        # jsonschema puts the missing property name in error.message; extract it.
        missing = error.message.replace("'", "").split(" ")[0]
        return ValidationError(
            f"{missing} is required for tool '{tool_name}'.",
            code="MISSING_REQUIRED_PROPERTY",
            parameter=missing,
            received=None,
            valid_range=None,
            suggestion=f"Provide the '{missing}' parameter. See the tool's inputSchema.",
        )

    if validator == "type":
        expected = validator_value if isinstance(validator_value, str) else "/".join(validator_value)
        return ValidationError(
            f"Parameter '{leaf_parameter}' must be {expected}, got {type(received).__name__}.",
            code="TYPE_ERROR",
            parameter=leaf_parameter,
            received=received,
            valid_range=expected,
            suggestion=f"Pass {leaf_parameter} as a {expected} value.",
        )

    if validator in ("minimum", "exclusiveMinimum"):
        rng = _numeric_bounds(leaf_schema)
        return ValidationError(
            f"Parameter '{leaf_parameter}' value {received} is below the minimum {validator_value}.",
            code="PARAM_OUT_OF_RANGE",
            parameter=leaf_parameter,
            received=_as_number(received),
            valid_range=rng,
            suggestion=_range_suggestion(leaf_parameter, rng),
        )

    if validator in ("maximum", "exclusiveMaximum"):
        rng = _numeric_bounds(leaf_schema)
        return ValidationError(
            f"Parameter '{leaf_parameter}' value {received} is above the maximum {validator_value}.",
            code="PARAM_OUT_OF_RANGE",
            parameter=leaf_parameter,
            received=_as_number(received),
            valid_range=rng,
            suggestion=_range_suggestion(leaf_parameter, rng),
        )

    if validator in ("enum",):
        return ValidationError(
            f"Parameter '{leaf_parameter}' value {received!r} is not one of the allowed values.",
            code="ENUM_VIOLATION",
            parameter=leaf_parameter,
            received=received,
            valid_range=list(validator_value),
            suggestion=f"Use one of: {', '.join(map(str, validator_value))}.",
        )

    # Any other keyword (pattern, minItems, etc.) -- still structured, generic code.
    return ValidationError(
        f"Parameter '{leaf_parameter}' failed validation: {error.message}.",
        code="SCHEMA_VIOLATION",
        parameter=leaf_parameter,
        received=received,
        valid_range=None,
        suggestion="See the tool's inputSchema for the exact constraint.",
    )


def _numeric_bounds(leaf_schema: dict[str, Any]) -> list[float] | None:
    """Recover [min, max] from a leaf schema. Tolerates exclusiveMinimum etc."""
    lo = leaf_schema.get("minimum", leaf_schema.get("exclusiveMinimum"))
    hi = leaf_schema.get("maximum", leaf_schema.get("exclusiveMaximum"))
    if lo is None or hi is None:
        return None
    return [float(lo), float(hi)]


def _range_suggestion(parameter: str, rng: list[float] | None) -> str:
    if rng is None or len(rng) != 2:
        return CORRECTIVE_ACTIONS["PARAM_OUT_RANGE"].format(min="", max="")
    return f"Use a value between {rng[0]} and {rng[1]} for '{parameter}'."


def _as_number(value: Any) -> Any:
    """jsonschema hands us the raw instance; keep numerics as-is for received."""
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value
    # Non-numeric (shouldn't happen on minimum/maximum, but be safe).
    return value
