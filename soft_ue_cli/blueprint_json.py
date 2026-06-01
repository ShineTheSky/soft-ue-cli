"""Validate and process Blueprint JSON descriptions.

Supports two operations:
  validate-blueprint-json  – structural + semantic checks on a JSON file
  create-blueprint-from-json – sends the validated JSON to the UE bridge for creation
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# ── Known node types and their standard pins ──────────────────────────────────
#
# Each entry maps pin categories: "exec" (execution flow), "data" (typed data).
# direction:  "in" = input to the node, "out" = output from the node.
#
# This is used for structural validation only – it won't catch every edge case
# but will flag the most common AI generation mistakes.

KNOWN_NODE_TYPES: dict[str, dict[str, list[dict[str, str]]]] = {
    "K2Node_Event": {
        "out": [
            {"name": "then", "category": "exec"},
        ],
        # Native events add output data pins per their signature; we skip
        # exact matching here to avoid false positives.
    },
    "K2Node_CustomEvent": {
        "out": [
            {"name": "then", "category": "exec"},
        ],
    },
    "K2Node_CallFunction": {
        "in": [
            {"name": "execute", "category": "exec"},
        ],
        "out": [
            {"name": "then", "category": "exec"},
        ],
    },
    "K2Node_LatentAbilityCall": {
        "in": [
            {"name": "execute", "category": "exec"},
        ],
        "out": [
            # outputs vary by task type (OnCompleted, OnBlendOut, etc.)
        ],
    },
    "K2Node_IfThenElse": {
        "in": [
            {"name": "execute", "category": "exec"},
            {"name": "Condition", "category": "data"},
        ],
        "out": [
            {"name": "then", "category": "exec"},
            {"name": "else", "category": "exec"},
        ],
    },
    "K2Node_VariableGet": {
        "out": [
            {"name": "ReturnValue", "category": "data"},
        ],
    },
    "K2Node_VariableSet": {
        "in": [
            {"name": "execute", "category": "exec"},
            {"name": "value", "category": "data"},
        ],
        "out": [
            {"name": "then", "category": "exec"},
            {"name": "ReturnValue", "category": "data"},
        ],
    },
    "K2Node_DynamicCast": {
        "in": [
            {"name": "execute", "category": "exec"},
            {"name": "Object", "category": "data"},
        ],
        "out": [
            {"name": "then", "category": "exec"},
            {"name": "failed", "category": "exec"},
        ],
    },
    "K2Node_MakeArray": {
        "in": [],
        "out": [
            {"name": "Array", "category": "data"},
        ],
    },
    "K2Node_ForEachLoop": {
        "in": [
            {"name": "execute", "category": "exec"},
            {"name": "Array", "category": "data"},
        ],
        "out": [
            {"name": "LoopBody", "category": "exec"},
            {"name": "Completed", "category": "exec"},
            {"name": "Index", "category": "data"},
            {"name": "Element", "category": "data"},
        ],
    },
    "K2Node_Self": {
        "out": [
            {"name": "self", "category": "data"},
        ],
    },
    "K2Node_Literal": {
        "out": [
            {"name": "ReturnValue", "category": "data"},
        ],
    },
    "K2Node_MakeStruct": {
        "in": [],
        "out": [
            {"name": "ReturnValue", "category": "data"},
        ],
    },
    "K2Node_BreakStruct": {
        "in": [
            {"name": "execute", "category": "exec"},
            {"name": "Struct", "category": "data"},
        ],
        "out": [
            {"name": "then", "category": "exec"},
        ],
    },
    "K2Node_Select": {
        "in": [
            {"name": "Index", "category": "data"},
        ],
        "out": [
            {"name": "ReturnValue", "category": "data"},
        ],
    },
    "K2Node_ExecutionSequence": {
        "in": [
            {"name": "execute", "category": "exec"},
        ],
        "out": [
            {"name": "then_0", "category": "exec"},
            {"name": "then_1", "category": "exec"},
        ],
    },
}


# Node types that are **not** K2 nodes but are common enough to accept without
# flagging as unrecognised.
ALIASED_TYPES: dict[str, str] = {
    "Event": "K2Node_Event",
    "CustomEvent": "K2Node_CustomEvent",
    "CallFunction": "K2Node_CallFunction",
    "LatentCall": "K2Node_LatentAbilityCall",
    "Branch": "K2Node_IfThenElse",
    "IfThenElse": "K2Node_IfThenElse",
    "If": "K2Node_IfThenElse",
    "VariableGet": "K2Node_VariableGet",
    "Getter": "K2Node_VariableGet",
    "VariableSet": "K2Node_VariableSet",
    "Setter": "K2Node_VariableSet",
    "DynamicCast": "K2Node_DynamicCast",
    "Cast": "K2Node_DynamicCast",
    "CastTo": "K2Node_DynamicCast",
    "MakeArray": "K2Node_MakeArray",
    "ForEachLoop": "K2Node_ForEachLoop",
    "ForLoop": "K2Node_ForEachLoop",
    "Self": "K2Node_Self",
    "Literal": "K2Node_Literal",
    "MakeStruct": "K2Node_MakeStruct",
    "BreakStruct": "K2Node_BreakStruct",
    "Select": "K2Node_Select",
    "Sequence": "K2Node_ExecutionSequence",
    "ExecutionSequence": "K2Node_ExecutionSequence",
}

EXEC_CATEGORIES = frozenset({"exec", "delegate"})
DATA_CATEGORIES = frozenset({
    "bool", "byte", "int", "int64", "float", "real", "double",
    "string", "name", "text", "object", "class", "soft_object",
    "soft_class", "struct", "enum", "interface", "wildcard",
})


@dataclass
class ValidationError:
    message: str
    path: str = ""  # e.g. "nodes[2].id" or "connections[5]"
    severity: str = "error"  # "error" or "warning"


@dataclass
class ValidationResult:
    errors: list[ValidationError] = field(default_factory=list)
    warnings: list[ValidationError] = field(default_factory=list)

    @property
    def is_valid(self) -> bool:
        return len(self.errors) == 0

    def error(self, message: str, path: str = "") -> None:
        self.errors.append(ValidationError(message, path, "error"))

    def warning(self, message: str, path: str = "") -> None:
        self.warnings.append(ValidationError(message, path, "warning"))

    def has_error_at(self, prefix: str) -> bool:
        return any(e.path.startswith(prefix) for e in self.errors)


def validate_blueprint_json(data: dict[str, Any]) -> ValidationResult:
    """Run all validation checks on a blueprint JSON dict.

    Returns a ValidationResult with .errors and .warnings.
    """
    result = ValidationResult()

    # ── top-level structure ──────────────────────────────────────────────
    _validate_top_level(data, result)
    if result.has_error_at("top"):
        return result  # can't continue without valid top-level

    # ── nodes ────────────────────────────────────────────────────────────
    node_ids: set[str] = set()
    _validate_nodes(data.get("nodes", []), data.get("variables", []), node_ids, result)

    # ── connections ──────────────────────────────────────────────────────
    _validate_connections(data.get("connections", []), node_ids, data.get("nodes", []), result)

    # ── variables ────────────────────────────────────────────────────────
    _validate_variables(data.get("variables", []), result)

    return result


def validate_blueprint_json_file(path: str | Path) -> ValidationResult:
    """Load and validate a blueprint JSON file."""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return validate_blueprint_json(data)


def _validate_top_level(data: dict[str, Any], result: ValidationResult) -> None:
    if not isinstance(data, dict):
        result.error("JSON root must be an object", "top")
        return

    if "parent_class" not in data:
        result.error("Missing required field: parent_class", "top.parent_class")
    elif not isinstance(data["parent_class"], str) or not data["parent_class"].strip():
        result.error("parent_class must be a non-empty string", "top.parent_class")

    if "nodes" not in data:
        result.error("Missing required field: nodes", "top.nodes")
    elif not isinstance(data["nodes"], list):
        result.error("nodes must be an array", "top.nodes")
    elif len(data["nodes"]) == 0:
        result.error("nodes array must not be empty", "top.nodes")

    if "connections" in data:
        if not isinstance(data["connections"], list):
            result.error("connections must be an array", "top.connections")

    if "variables" in data:
        if not isinstance(data["variables"], list):
            result.error("variables must be an array", "top.variables")


def _validate_nodes(
    nodes: list[dict[str, Any]],
    variables: list[dict[str, Any]],
    node_ids: set[str],
    result: ValidationResult,
) -> None:
    if not nodes:
        return

    var_names = {v.get("name", "") for v in variables if isinstance(v, dict)}

    for i, node in enumerate(nodes):
        prefix = f"nodes[{i}]"
        if not isinstance(node, dict):
            result.error(f"Each node must be an object", prefix)
            continue

        # id
        node_id = node.get("id", "")
        if not node_id:
            result.error(f"Node is missing required field: id", prefix)
        elif not isinstance(node_id, str):
            result.error(f"Node id must be a string", f"{prefix}.id")
        elif not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", node_id):
            result.error(
                f"Node id '{node_id}' must be alphanumeric + underscore, starting with a letter",
                f"{prefix}.id",
            )
        elif node_id in node_ids:
            result.error(f"Duplicate node id: '{node_id}'", f"{prefix}.id")
        else:
            node_ids.add(node_id)

        # type
        node_type = node.get("type", "")
        if not node_type:
            result.error(f"Node '{node_id}' is missing required field: type", f"{prefix}.type")
        elif not isinstance(node_type, str):
            result.error(f"Node type must be a string", f"{prefix}.type")
        else:
            resolved = ALIASED_TYPES.get(node_type, node_type)
            if resolved not in KNOWN_NODE_TYPES:
                result.warning(
                    f"Unrecognised node type '{node_type}'. "
                    f"Known types: {sorted(KNOWN_NODE_TYPES.keys())}",
                    f"{prefix}.type",
                )

        # function (for CallFunction / LatentCall)
        if node_type in {"K2Node_CallFunction", "K2Node_LatentAbilityCall", "CallFunction", "LatentCall"}:
            func_name = node.get("function", "")
            if not func_name:
                result.warning(
                    f"CallFunction node '{node_id}' has no 'function' field",
                    f"{prefix}.function",
                )

        # event (for Event / CustomEvent)
        if node_type in {"K2Node_Event", "K2Node_CustomEvent", "Event", "CustomEvent"}:
            event_name = node.get("event", "")
            if not event_name:
                result.warning(
                    f"Event node '{node_id}' has no 'event' field",
                    f"{prefix}.event",
                )

        # variable (for VariableGet / Set)
        if node_type in {"K2Node_VariableGet", "K2Node_VariableSet", "VariableGet", "VariableSet"}:
            var_name = node.get("variable", "")
            if not var_name:
                result.warning(
                    f"Variable node '{node_id}' has no 'variable' field",
                    f"{prefix}.variable",
                )
            elif var_name not in var_names:
                result.error(
                    f"Variable '{var_name}' referenced by node '{node_id}' "
                    f"is not declared in the variables list",
                    f"{prefix}.variable",
                )

        # cast_to (for DynamicCast)
        if node_type in {"K2Node_DynamicCast", "DynamicCast", "Cast", "CastTo"}:
            if not node.get("cast_to", ""):
                result.warning(
                    f"DynamicCast node '{node_id}' has no 'cast_to' field",
                    f"{prefix}.cast_to",
                )

        # position
        pos = node.get("position")
        if pos is not None:
            if not (isinstance(pos, list) and len(pos) == 2 and all(isinstance(v, (int, float)) for v in pos)):
                result.warning(
                    f"Node position must be [x, y] array of numbers",
                    f"{prefix}.position",
                )

        # defaults
        defaults = node.get("defaults")
        if defaults is not None and not isinstance(defaults, dict):
            result.error(f"Node defaults must be an object", f"{prefix}.defaults")


def _validate_connections(
    connections: list[Any],
    node_ids: set[str],
    nodes: list[dict[str, Any]],
    result: ValidationResult,
) -> None:
    if not connections:
        return

    # Build node type lookup
    node_types: dict[str, str] = {}
    for node in nodes:
        nid = node.get("id", "")
        ntype = node.get("type", "")
        resolved = ALIASED_TYPES.get(ntype, ntype)
        node_types[nid] = resolved

    for i, conn in enumerate(connections):
        prefix = f"connections[{i}]"
        if not isinstance(conn, (list, dict)):
            result.error(f"Connection must be an array [from, to] or object {{from, to}}", prefix)
            continue

        if isinstance(conn, list):
            if len(conn) != 2:
                result.error(f"Connection array must have exactly 2 elements: [from, to]", prefix)
                continue
            from_str, to_str = str(conn[0]), str(conn[1])
        else:
            from_str = str(conn.get("from", ""))
            to_str = str(conn.get("to", ""))
            if not from_str or not to_str:
                result.error(f"Connection object must have 'from' and 'to' fields", prefix)
                continue

        # Parse "node_id.pin_name"
        from_match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\.(.+)$", from_str)
        to_match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\.(.+)$", to_str)

        if not from_match:
            result.error(
                f"Invalid connection source '{from_str}'. Use format 'node_id.pin_name'",
                prefix,
            )
            continue
        if not to_match:
            result.error(
                f"Invalid connection target '{to_str}'. Use format 'node_id.pin_name'",
                prefix,
            )
            continue

        from_node, from_pin = from_match.group(1), from_match.group(2)
        to_node, to_pin = to_match.group(1), to_match.group(2)

        if from_node not in node_ids:
            result.error(
                f"Connection source node '{from_node}' not found in nodes list",
                prefix,
            )
        if to_node not in node_ids:
            result.error(
                f"Connection target node '{to_node}' not found in nodes list",
                prefix,
            )

        # ── exec / data polarity check ───────────────────────────────────
        # Figure out whether each pin is exec or data based on the known type.
        from_cat = _pin_category(node_types.get(from_node, ""), from_pin, "from")
        to_cat = _pin_category(node_types.get(to_node, ""), to_pin, "to")

        if from_cat and to_cat and from_cat != to_cat:
            result.error(
                f"Cannot connect a {from_cat}-type pin ('{from_str}') "
                f"to a {to_cat}-type pin ('{to_str}')",
                prefix,
            )


def _pin_category(node_type: str, pin_name: str, direction: str) -> str | None:
    """Return 'exec' or 'data' for a known pin, or None if unknown."""
    if not node_type or node_type not in KNOWN_NODE_TYPES:
        return None

    info = KNOWN_NODE_TYPES[node_type]
    # Check exec pins
    for entry in info.get("out" if direction == "from" else "in", []):
        if entry["name"].lower() == pin_name.lower():
            return "exec" if entry.get("category") == "exec" else "data"

    # Common patterns
    if pin_name in ("execute", "then", "else", "failed", "LoopBody", "Completed"):
        return "exec"
    if pin_name in ("Condition", "ReturnValue", "self", "Array", "Index", "Element"):
        return "data"

    return None


def _validate_variables(variables: list[dict[str, Any]], result: ValidationResult) -> None:
    seen: set[str] = set()
    for i, var in enumerate(variables):
        prefix = f"variables[{i}]"
        if not isinstance(var, dict):
            result.error(f"Each variable must be an object", prefix)
            continue

        name = var.get("name", "")
        if not name:
            result.error(f"Variable is missing required field: name", prefix)
        elif name in seen:
            result.error(f"Duplicate variable name: '{name}'", f"{prefix}.name")
        else:
            seen.add(name)

        vtype = var.get("type", "")
        if not vtype:
            result.error(f"Variable '{name}' is missing required field: type", f"{prefix}.type")
        elif vtype not in DATA_CATEGORIES and vtype not in ("object", "class", "struct", "enum"):
            result.warning(
                f"Variable '{name}' has unrecognised type '{vtype}'. "
                f"Expected: {sorted(DATA_CATEGORIES)}",
                f"{prefix}.type",
            )


def format_validation_result(result: ValidationResult) -> str:
    """Return a human-readable validation report."""
    lines: list[str] = []

    total_errors = len(result.errors)
    total_warnings = len(result.warnings)

    if total_errors == 0 and total_warnings == 0:
        lines.append("Validation passed – no errors or warnings.")

    for err in result.errors:
        loc = f" ({err.path})" if err.path else ""
        lines.append(f"ERROR{loc}: {err.message}")

    for warn in result.warnings:
        loc = f" ({warn.path})" if warn.path else ""
        lines.append(f"WARNING{loc}: {warn.message}")

    if total_errors > 0 or total_warnings > 0:
        lines.append("")
        lines.append(f"{total_errors} error(s), {total_warnings} warning(s)")

    return "\n".join(lines)
