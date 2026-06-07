# -*- coding: utf-8 -*-
"""BehaviorTree .bttxtv2 DSL <-> JSON compiler.

The v2 format stays as an indented BehaviorTree DSL, not YAML. Indentation
defines tree ownership, while properties are written as explicit blocks:

    behaviortree BT_KeepDistance:
        blackboard: /Game/AI/Blackboard/BB_KeepDistance.BB_KeepDistance
            key Player: Object

        Root: Selector
            # @intent
            Sequence
                # @summary
                # @intent
                Decorator: BTDecorator_Blackboard
                    FlowControl: Both
                    properties:
                        BlackboardKey: $Player

                # @summary
                # @intent
                Task: /Game/AI/Tasks/AITask_KeepDistance.AITask_KeepDistance_C
                    properties:
                        Player: $Player
                        SearchRadius: 200

Only fields that can be read from BehaviorTree/Blackboard assets are formal
DSL fields. Comments such as @summary and @intent are hints for humans/LLMs and
are ignored by the compiler.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


_INDENT_RE = re.compile(r"^([ \t]*)(\S.*)?$")
_BB_SELECTOR_RE = re.compile(r'^\(SelectedKeyName="([^"]+)",bNoneIsAllowedValue=(?:True|False|true|false)\)$')
_ENUM_INTERNAL_RE = re.compile(r"^NewEnumerator\d+$")
_BTTXT_INDENT = "\t"
_BLACKBOARD_ENUM_VALUE_PROPERTIES = {"StringValue"}

_LINE_MATCHERS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^behaviortree\s+(\w+)\s*:\s*$"), "behaviortree"),
    (re.compile(r"^blackboard:\s*(\S+)\s*$"), "blackboard_ref"),
    (re.compile(r"^key\s+(\w[\w.]*)\s*:\s*(\w+)\s*(.*?)\s*(\[.*\])?$"), "key"),
    (re.compile(r"^Root:\s*(Selector|Sequence|SimpleParallel|Simple Parallel)\s*$"), "root"),
    (re.compile(r"^(Selector|Sequence|SimpleParallel|Simple Parallel)\s*$"), "composite"),
    (re.compile(r"^Task:\s*(\S+)\s*$"), "task"),
    (re.compile(r"^Decorator:\s*(\S+)\s*$"), "decorator"),
    (re.compile(r"^Service:\s*(\S+)\s*$"), "service"),
    (re.compile(r"^RunBehaviorTree:\s*(\S+)\s*$"), "run_subtree"),
    (re.compile(r"^FlowControl:\s*(\w+)\s*$"), "flow_control"),
    (re.compile(r"^properties:\s*$"), "properties_block"),
    (re.compile(r"^([^:#][^:]*):\s*(.*)$"), "property"),
]


@dataclass
class BTParseError(Exception):
    msg: str
    line_no: int = 0

    def __str__(self) -> str:
        loc = f"line {self.line_no}: " if self.line_no else ""
        return f"{loc}{self.msg}"


@dataclass
class BTToken:
    kind: str
    line_no: int
    indent: int
    raw: str
    groups: tuple[str, ...] = ()

    def group(self, index: int) -> str:
        return self.groups[index] if index < len(self.groups) else ""


def _strip_inline_comment(line: str) -> str:
    in_single = False
    in_double = False
    escaped = False
    out: list[str] = []
    for ch in line:
        if escaped:
            out.append(ch)
            escaped = False
            continue
        if ch == "\\":
            out.append(ch)
            escaped = True
            continue
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch == "#" and not in_single and not in_double:
            break
        out.append(ch)
    return "".join(out)


def tokenize_bt(text: str) -> list[BTToken]:
    tokens: list[BTToken] = []
    for line_no, raw_line in enumerate(text.splitlines(), start=1):
        if raw_line.lstrip().startswith("#"):
            continue
        cleaned = _strip_inline_comment(raw_line).rstrip()
        if not cleaned.strip():
            continue

        m = _INDENT_RE.match(cleaned)
        indent = len(m.group(1).replace("\t", "    ")) if m else 0
        content = m.group(2) if m and m.group(2) else ""
        if not content:
            continue

        for pattern, kind in _LINE_MATCHERS:
            match = pattern.match(content)
            if match:
                tokens.append(BTToken(kind, line_no, indent, content, match.groups()))
                break
        else:
            raise BTParseError(f"Unrecognized bttxtv2 line: {content}", line_no)
    return tokens


def _normalize_class_ref(raw: str) -> str:
    return raw.strip()


def _normalize_property_value(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        value = value[1:-1]
    if value.startswith("$") and len(value) > 1:
        key = value[1:]
        return f'(SelectedKeyName="{key}",bNoneIsAllowedValue=False)'
    return value


def _shorten_property_value(value: Any, enum_internal_to_display: dict[str, str] | None = None) -> str:
    text = str(value).strip()
    bb_match = _BB_SELECTOR_RE.match(text)
    if bb_match:
        return f"${bb_match.group(1)}"
    if enum_internal_to_display:
        return enum_internal_to_display.get(text, text)

    lowered = text.lower()
    if lowered == "true":
        return "true"
    if lowered == "false":
        return "false"
    if re.match(r"^[+-]?\d+\.\d+$", text):
        text = text.rstrip("0").rstrip(".")
        return text or "0"
    return text


def _parse_key_params(raw: str) -> dict[str, str]:
    params: dict[str, str] = {}
    for part in raw.split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        params[key.strip().lower()] = value.strip().strip('"').strip("'")
    return params


def _parse_enum_values(raw: str) -> list[dict[str, str]]:
    raw = raw.strip()
    if not raw:
        return []
    if raw[0] in "[(" and raw[-1] in "])":
        raw = raw[1:-1]
    entries: list[dict[str, str]] = []
    for part in raw.split(","):
        item = part.strip()
        if not item or ":" not in item:
            continue
        internal, display = item.split(":", 1)
        internal = internal.strip()
        display = display.strip()
        if internal and display:
            entries.append({"internal_name": internal, "display_name": display, "authored_name": display})
    return entries


def _enum_entries_from_key(key: dict[str, Any]) -> list[dict[str, str]]:
    raw_entries = key.get("enum_entries") or key.get("enumerators") or key.get("enum_values") or []
    entries: list[dict[str, str]] = []
    if isinstance(raw_entries, list):
        for entry in raw_entries:
            if not isinstance(entry, dict):
                continue
            internal = str(entry.get("internal_name") or "")
            display = str(entry.get("display_name") or entry.get("authored_name") or "")
            if internal and display:
                entries.append({"internal_name": internal, "display_name": display, "authored_name": display})
    return entries


def _enum_maps_by_key(blackboard: dict[str, Any]) -> dict[str, dict[str, dict[str, str]]]:
    maps: dict[str, dict[str, dict[str, str]]] = {}
    for key in blackboard.get("keys", []):
        if _key_type_short(key.get("type", "")).lower() != "enum":
            continue
        entries = _enum_entries_from_key(key)
        if not entries:
            continue
        internal_to_display: dict[str, str] = {}
        display_to_internal: dict[str, str] = {}
        for entry in entries:
            internal = entry["internal_name"]
            display = entry["display_name"]
            internal_to_display[internal] = display
            display_to_internal[display.lower()] = internal
            display_to_internal[internal.lower()] = internal
        maps[str(key.get("name", ""))] = {
            "internal_to_display": internal_to_display,
            "display_to_internal": display_to_internal,
        }
    return maps


def _selector_key_name(value: Any) -> str:
    match = _BB_SELECTOR_RE.match(str(value).strip())
    return match.group(1) if match else ""


def _enum_key_for_property(prop_name: str, props: dict[str, Any], enum_maps: dict[str, dict[str, dict[str, str]]]) -> str:
    bb_key = _selector_key_name(props.get("BlackboardKey", ""))
    if prop_name in _BLACKBOARD_ENUM_VALUE_PROPERTIES and bb_key in enum_maps:
        return bb_key

    for key_name in enum_maps:
        if prop_name.lower() == f"{key_name}enum".lower():
            return key_name

    if len(enum_maps) == 1 and _ENUM_INTERNAL_RE.match(str(props.get(prop_name, "")).strip()):
        return next(iter(enum_maps))
    return ""


def _token_to_node(token: BTToken) -> dict[str, Any]:
    if token.kind in ("root", "composite"):
        return {
            "kind": "Root" if token.kind == "root" else "Composite",
            "type": token.group(0) if token.kind == "root" else token.raw,
        }
    if token.kind == "task":
        return {"kind": "Task", "class": _normalize_class_ref(token.group(0)), "properties": {}}
    if token.kind == "decorator":
        return {"kind": "Decorator", "class": _normalize_class_ref(token.group(0)), "properties": {}}
    if token.kind == "service":
        return {"kind": "Service", "class": _normalize_class_ref(token.group(0)), "properties": {}}
    if token.kind == "run_subtree":
        return {"kind": "RunBehaviorTree", "subtree_path": token.group(0)}
    raise BTParseError(f"Cannot create node from {token.kind}", token.line_no)


def compile_bt_text_to_json(text: str) -> dict[str, Any]:
    tokens = tokenize_bt(text)
    if not tokens or tokens[0].kind != "behaviortree":
        raise BTParseError("Must start with 'behaviortree Name:'", tokens[0].line_no if tokens else 0)

    bt_name = tokens[0].group(0)
    blackboard_name = ""
    blackboard_path = ""
    keys: list[dict[str, Any]] = []
    root_node: dict[str, Any] | None = None

    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token.kind == "blackboard_ref":
            value = token.group(0)
            if value.startswith("/"):
                blackboard_path = value
            else:
                blackboard_name = value
        elif token.kind == "key":
            key_name = token.group(0)
            key_type = token.group(1)
            params = _parse_key_params(token.group(2) or "")
            key: dict[str, Any] = {"name": key_name, "type": key_type}
            if key_type.lower() == "enum":
                enum_path = params.get("enum") or params.get("enum_path") or params.get("enum_type") or ""
                if not enum_path:
                    raise BTParseError(f"Enum key {key_name} requires enum=/Game/...", token.line_no)
                key["enum_path"] = enum_path
                enum_values = params.get("values") or params.get("enum_values") or ""
                entries = _parse_enum_values(enum_values)
                if entries:
                    key["enum_entries"] = entries
            flags = token.group(3) or ""
            if "instanceeditable" in flags.lower() or "instance_editable" in flags.lower():
                key["instance_editable"] = True
            keys.append(key)
        elif token.kind == "root":
            root_node = _build_tree_stack(tokens[index:])
            break
        elif token.kind not in ("behaviortree",):
            raise BTParseError("Only blackboard/key declarations are allowed before Root", token.line_no)
        index += 1

    if not root_node:
        raise BTParseError("No root node found")

    result: dict[str, Any] = {
        "name": bt_name,
        "blackboard_name": _blackboard_name_from_path(blackboard_path) if blackboard_path else (blackboard_name or f"BB_{bt_name}"),
        "blackboard_path": blackboard_path,
        "root": root_node,
    }
    if keys:
        result["blackboard_keys"] = keys
        result["blackboard"] = {
            "name": result["blackboard_name"],
            "path": blackboard_path,
            "keys": keys,
        }
    _restore_enum_internal_values(result)
    return result


def _blackboard_name_from_path(path: str) -> str:
    if not path:
        return ""
    if "." in path:
        return path.rsplit(".", 1)[-1]
    return path.rsplit("/", 1)[-1]


def _indent(level: int) -> str:
    return _BTTXT_INDENT * level


def _build_tree_stack(tokens: list[BTToken]) -> dict[str, Any]:
    root: dict[str, Any] | None = None
    node_stack: list[tuple[int, dict[str, Any]]] = []
    prop_stack: list[tuple[int, dict[str, Any]]] = []

    for token in tokens:
        while prop_stack and token.indent <= prop_stack[-1][0]:
            prop_stack.pop()

        if token.kind == "properties_block":
            if not node_stack or token.indent <= node_stack[-1][0]:
                raise BTParseError("properties: must be nested under a node", token.line_no)
            prop_stack.append((token.indent, node_stack[-1][1]))
            node_stack[-1][1].setdefault("properties", {})
            continue

        if token.kind == "property":
            if not prop_stack or token.indent <= prop_stack[-1][0]:
                raise BTParseError("Property lines must be nested under properties:", token.line_no)
            key = token.group(0).strip()
            value = _normalize_property_value(token.group(1))
            prop_stack[-1][1].setdefault("properties", {})[key] = value
            continue

        while node_stack and token.indent <= node_stack[-1][0]:
            node_stack.pop()

        if token.kind == "flow_control":
            if not node_stack or token.indent <= node_stack[-1][0]:
                raise BTParseError("FlowControl must be nested under a Decorator", token.line_no)
            flow_control = token.group(0)
            node_stack[-1][1]["flow_control"] = flow_control
            node_stack[-1][1].setdefault("properties", {})["FlowAbortMode"] = flow_control
            continue

        if token.kind not in ("root", "composite", "task", "decorator", "service", "run_subtree"):
            raise BTParseError(f"Unexpected line in tree: {token.raw}", token.line_no)

        node = _token_to_node(token)
        if not node_stack:
            if token.kind != "root":
                raise BTParseError("Tree must begin with Root: <CompositeType>", token.line_no)
            root = node
            node_stack.append((token.indent, node))
            continue

        parent = node_stack[-1][1]
        if token.kind == "decorator":
            parent.setdefault("decorators", []).append(node)
        elif token.kind == "service":
            parent.setdefault("services", []).append(node)
        else:
            parent.setdefault("children", []).append(node)
        node_stack.append((token.indent, node))

    if root is None:
        raise BTParseError("No root node found")
    return root


def decompile_bt_json_to_text(data: dict[str, Any]) -> str:
    lines: list[str] = []
    name = data.get("name", "BT_New")
    lines.append(f"behaviortree {name}:")

    blackboard = data.get("blackboard", {})
    enum_maps = _enum_maps_by_key(blackboard)
    bb_path = blackboard.get("path", "")
    bb_name = blackboard.get("name", "")
    if bb_path:
        lines.append(f"{_indent(1)}blackboard: {bb_path}")
    elif bb_name:
        lines.append(f"{_indent(1)}blackboard: {bb_name}")

    for key in blackboard.get("keys", []):
        inst = " [InstanceEditable]" if key.get("instance_editable") or key.get("is_instance_editable") else ""
        key_type = _key_type_short(key.get("type", "Object"))
        extra = ""
        if key_type == "Enum":
            extra = f" enum={key.get('enum_path') or key.get('enum_type') or key.get('enum') or ''}"
            entries = _enum_entries_from_key(key)
            if entries:
                values = ",".join(f"{entry['internal_name']}:{entry['display_name']}" for entry in entries)
                extra += f" values=({values})"
        lines.append(f"{_indent(2)}key {key['name']}: {key_type}{extra}{inst}")

    root = data.get("root", {})
    if root:
        lines.append("")
        _decompile_node(root, lines, indent=1, enum_maps=enum_maps)
    return "\n".join(lines)


def _key_type_short(type_name: str) -> str:
    mapping = {
        "BlackboardKeyType_Object": "Object",
        "BlackboardKeyType_Bool": "Bool",
        "BlackboardKeyType_Float": "Float",
        "BlackboardKeyType_Int": "Int",
        "BlackboardKeyType_Vector": "Vector",
        "BlackboardKeyType_String": "String",
        "BlackboardKeyType_Enum": "Enum",
    }
    for ue_name, short in mapping.items():
        if ue_name.lower() in str(type_name).lower():
            return short
    return str(type_name).replace("BlackboardKeyType_", "")


def _class_ref(node: dict[str, Any]) -> str:
    return str(node.get("class_path") or node.get("class") or "Unknown")


def _emit_annotation(lines: list[str], indent: int, kind: str) -> None:
    prefix = _indent(indent)
    if kind in ("Task", "Decorator", "Service"):
        lines.append(f"{prefix}# @summary")
        lines.append(f"{prefix}# @intent")
    elif kind in ("Composite", "RunBehaviorTree"):
        lines.append(f"{prefix}# @intent")


def _with_kind(node: dict[str, Any], kind: str) -> dict[str, Any]:
    if node.get("kind"):
        return node
    copy = dict(node)
    copy["kind"] = kind
    return copy


def _decompile_node(
    node: dict[str, Any],
    lines: list[str],
    indent: int,
    enum_maps: dict[str, dict[str, dict[str, str]]] | None = None,
) -> None:
    prefix = _indent(indent)
    kind = node.get("kind", "Unknown")

    if kind in ("Root", "Composite"):
        node_type = node.get("type", "Selector")
        if kind == "Root":
            lines.append(f"{prefix}Root: {node_type}")
        else:
            _emit_annotation(lines, indent, "Composite")
            lines.append(f"{prefix}{node_type}")

        for decorator in node.get("decorators", []):
            _decompile_node(_with_kind(decorator, "Decorator"), lines, indent + 1, enum_maps)
        for service in node.get("services", []):
            _decompile_node(_with_kind(service, "Service"), lines, indent + 1, enum_maps)
        for child in node.get("children", []):
            _decompile_node(child, lines, indent + 1, enum_maps)
        return

    if kind == "Decorator":
        _emit_annotation(lines, indent, "Decorator")
        lines.append(f"{prefix}Decorator: {_class_ref(node)}")
        flow_control = node.get("flow_control")
        if flow_control and flow_control != "None":
            lines.append(f"{prefix}{_indent(1)}FlowControl: {flow_control}")
        _decompile_properties(node.get("properties", {}), lines, indent + 1, enum_maps)
        return

    if kind == "Service":
        _emit_annotation(lines, indent, "Service")
        lines.append(f"{prefix}Service: {_class_ref(node)}")
        _decompile_properties(node.get("properties", {}), lines, indent + 1, enum_maps)
        return

    if kind == "Task":
        _emit_annotation(lines, indent, "Task")
        lines.append(f"{prefix}Task: {_class_ref(node)}")
        _decompile_properties(node.get("properties", {}), lines, indent + 1, enum_maps)
        for decorator in node.get("decorators", []):
            _decompile_node(_with_kind(decorator, "Decorator"), lines, indent + 1, enum_maps)
        return

    if kind == "RunBehaviorTree":
        _emit_annotation(lines, indent, "RunBehaviorTree")
        lines.append(f"{prefix}RunBehaviorTree: {node.get('subtree_path', '')}")


def _decompile_properties(
    props: dict[str, Any],
    lines: list[str],
    indent: int,
    enum_maps: dict[str, dict[str, dict[str, str]]] | None = None,
) -> None:
    if not props:
        return
    prefix = _indent(indent)
    filtered_props = {key: value for key, value in props.items() if key != "FlowAbortMode"}
    if not filtered_props:
        return
    lines.append(f"{prefix}properties:")
    for key, value in filtered_props.items():
        enum_key = _enum_key_for_property(key, filtered_props, enum_maps or {})
        enum_map = (enum_maps or {}).get(enum_key, {}).get("internal_to_display", {})
        lines.append(f"{prefix}{_indent(1)}{key}: {_shorten_property_value(value, enum_map)}")


def _restore_enum_internal_values(data: dict[str, Any], *, strict: bool = True) -> None:
    enum_maps = _enum_maps_by_key(data.get("blackboard", {}))
    if not enum_maps:
        return

    def visit(node: dict[str, Any]) -> None:
        props = node.get("properties")
        if isinstance(props, dict):
            for prop_name, value in list(props.items()):
                enum_key = _enum_key_for_property(prop_name, props, enum_maps)
                if not enum_key:
                    continue
                display_to_internal = enum_maps[enum_key].get("display_to_internal", {})
                text = str(value).strip()
                internal = display_to_internal.get(text.lower())
                if internal:
                    props[prop_name] = internal
                elif strict:
                    valid_values = sorted(
                        {
                            display
                            for display in enum_maps[enum_key].get("internal_to_display", {}).values()
                            if display
                        }
                    )
                    valid_hint = ", ".join(valid_values) if valid_values else "no enum values discovered"
                    raise BTParseError(
                        f"Invalid enum value '{text}' for property '{prop_name}' using blackboard key "
                        f"'{enum_key}'. Expected one of: {valid_hint}"
                    )
        for collection in ("decorators", "services", "children"):
            for child in node.get(collection, []) or []:
                if isinstance(child, dict):
                    visit(child)

    root = data.get("root")
    if isinstance(root, dict):
        visit(root)


@dataclass
class BTValidationIssue:
    message: str
    line_no: int = 0
    severity: str = "error"


@dataclass
class BTValidationResult:
    issues: list[BTValidationIssue] = field(default_factory=list)

    @property
    def errors(self) -> list[BTValidationIssue]:
        return [issue for issue in self.issues if issue.severity == "error"]

    @property
    def warnings(self) -> list[BTValidationIssue]:
        return [issue for issue in self.issues if issue.severity == "warning"]

    @property
    def is_valid(self) -> bool:
        return not self.errors


def validate_bt_text(text: str) -> BTValidationResult:
    result = BTValidationResult()
    try:
        payload = compile_bt_text_to_json(text)
    except BTParseError as exc:
        result.issues.append(BTValidationIssue(exc.msg, exc.line_no, "error"))
        return result
    except Exception as exc:
        result.issues.append(BTValidationIssue(str(exc), 0, "error"))
        return result

    if not _contains_kind(payload.get("root", {}), "Task"):
        result.issues.append(BTValidationIssue("BehaviorTree has no Task nodes", 0, "warning"))
    return result


def _contains_kind(node: dict[str, Any], kind: str) -> bool:
    if node.get("kind") == kind:
        return True
    for section in ("decorators", "services", "children"):
        for child in node.get(section, []):
            if _contains_kind(child, kind):
                return True
    return False


def _strip_bt_frontmatter(text: str) -> tuple[str, dict[str, str]]:
    text = text.lstrip("\ufeff")
    meta: dict[str, str] = {}
    if text.startswith("---"):
        end = text.find("---", 3)
        if end != -1:
            frontmatter = text[3:end].strip()
            for line in frontmatter.splitlines():
                if ":" in line:
                    key, value = line.split(":", 1)
                    meta[key.strip()] = value.strip()
            text = text[end + 3:].strip()
    return text, meta


def compile_bt_file(bt_path: str | Path) -> dict[str, Any]:
    with open(bt_path, "r", encoding="utf-8") as handle:
        raw = handle.read()
    text, meta = _strip_bt_frontmatter(raw)
    payload = compile_bt_text_to_json(text)
    asset_path = meta.get("asset", "").strip()
    if asset_path:
        payload["asset_path"] = asset_path
    return payload


def decompile_bt_file(json_path: str | Path) -> str:
    with open(json_path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    return decompile_bt_json_to_text(data)


def validate_bt_file(bt_path: str | Path) -> BTValidationResult:
    with open(bt_path, "r", encoding="utf-8") as handle:
        raw = handle.read()
    text, _ = _strip_bt_frontmatter(raw)
    return validate_bt_text(text)
