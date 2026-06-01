# -*- coding: utf-8 -*-
"""BehaviorTree indented-text <-> JSON compiler.

Text format (".bttxt" files):

    behaviortree BT_Enemy:
        blackboard: BB_Enemy
            key TargetActor: Object
            key HomeLocation: Vector
            key AlertLevel: Float [InstanceEditable]
            key IsAggressive: Bool

        Root: Selector
            Service: BTService_UpdatePerception (Interval=0.5)
            Sequence
                Decorator: BTDecorator_Blackboard (Key=IsAggressive)
                Task: BTTask_MoveTo (Key=TargetActor, SpeedFactor=1.5)
                    Decorator: BTDecorator_IsInRange (Range=300,
                        FlowControl=AbortSelf)
                Task: BTTask_Wait (Time=3)

Rules:
  - # comments
  - Indentation defines nesting (4 spaces per level)
  - Each line: [Type]: [ClassName] [(properties)]
  - Types: Root, Selector, Sequence, SimpleParallel, Task, Decorator, Service
  - Properties in (): Key=Value, Key2: Value2, separated by commas
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


# ── Tokenizer ───────────────────────────────────────────────────────────────

_COMMENT_RE = re.compile(r"#.*$")
_INDENT_RE = re.compile(r"^([ \t]*)(\S.*)?$")

# Line-type matchers
_LINE_MATCHERS: list[tuple[re.Pattern, str]] = [
    (re.compile(r"^behaviortree\s+(\w+)\s*:\s*$"), "behaviortree"),
    (re.compile(r"^blackboard:\s*([/\w][\w/.]*)\s*$"), "blackboard_ref"),
    (re.compile(r"^key\s+(\w[\w.]*)\s*:\s*(\w+)\s*(\[.*\])?$"), "key"),
    (re.compile(r"^Root:\s*(\w+)\s*$"), "root"),
    (re.compile(r"^(Selector|Sequence|SimpleParallel|Simple Parallel)\s*$"), "composite"),
    (re.compile(r"^Task:\s*([/\w][\w./]*)\s*(?:\((.*)\))?\s*$"), "task"),
    (re.compile(r"^Decorator:\s*([/\w][\w./]*)\s*(?:\((.*)\))?\s*$"), "decorator"),
    (re.compile(r"^Service:\s*([/\w][\w./]*)\s*(?:\((.*)\))?\s*$"), "service"),
    (re.compile(r"^RunBehaviorTree:\s*(\w[\w./]*)\s*$"), "run_subtree"),
]


@dataclass
class BTToken:
    kind: str
    line_no: int
    indent: int
    raw: str
    groups: tuple = ()

    def group(self, i: int) -> str:
        return self.groups[i] if i < len(self.groups) else ""


def tokenize_bt(text: str) -> list[BTToken]:
    tokens: list[BTToken] = []
    for line_no, raw_line in enumerate(text.split("\n"), start=1):
        cleaned = _COMMENT_RE.sub("", raw_line)
        if not cleaned.strip():
            continue

        m = _INDENT_RE.match(cleaned)
        indent = len(m.group(1)) if m else 0
        content = m.group(2) if m and m.group(2) else ""

        if not content:
            continue

        for pattern, kind in _LINE_MATCHERS:
            pm = pattern.match(content)
            if pm:
                tokens.append(BTToken(kind=kind, line_no=line_no, indent=indent,
                                       raw=content, groups=pm.groups()))
                break
    return tokens


# ── Args parser ─────────────────────────────────────────────────────────────

def _parse_bt_args(s: str | None) -> dict[str, str]:
    """Parse 'Key=Val, Key2: Val2, Flag' into dict."""
    if not s:
        return {}
    result: dict[str, str] = {}
    # Split by comma respecting nested parens
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        parts.append("".join(current).strip())

    for part in parts:
        part = part.strip()
        if not part:
            continue
        sep = "=" if "=" in part else (":" if ":" in part else None)
        if sep:
            k, v = part.split(sep, 1)
            result[k.strip()] = v.strip().strip('"').strip("'")
        else:
            result[part] = "true"  # flag-style param
    return result


# ── Text → JSON compiler ────────────────────────────────────────────────────

@dataclass
class BTParseError(Exception):
    msg: str
    line_no: int = 0

    def __str__(self):
        loc = f"line {self.line_no}: " if self.line_no else ""
        return f"{loc}{self.msg}"


def compile_bt_text_to_json(text: str) -> dict[str, Any]:
    """Compile indented text to BehaviorTree JSON."""
    tokens = tokenize_bt(text)
    if not tokens or tokens[0].kind != "behaviortree":
        raise BTParseError("Must start with 'behaviortree Name:'", tokens[0].line_no if tokens else 0)

    bt_name = tokens[0].group(0)
    blackboard_name = ""
    blackboard_path = ""
    keys: list[dict[str, Any]] = []
    root_node: dict[str, Any] | None = None

    idx = 1
    while idx < len(tokens):
        tok = tokens[idx]
        idx += 1

        if tok.kind == "blackboard_ref":
            val = tok.group(0)
            if val.startswith("/"):
                blackboard_path = val
            else:
                blackboard_name = val
        elif tok.kind == "key":
            key_name = tok.group(0)
            key_type = tok.group(1)
            flags_str = tok.group(2) or ""
            entry: dict[str, Any] = {"name": key_name, "type": key_type}
            if "InstanceEditable" in flags_str or "instance_editable" in flags_str.lower():
                entry["instance_editable"] = True
            keys.append(entry)
        elif tok.kind == "root":
            root_node = _build_tree_stack(tokens[idx - 1:])
            break

    if not root_node:
        raise BTParseError("No root node found")

    result: dict[str, Any] = {
        "name": bt_name,
        "blackboard_name": (blackboard_name or f"BB_{bt_name}") if not blackboard_path else "",
        "blackboard_path": blackboard_path,
        "root": root_node,
    }
    if keys:
        result["blackboard_keys"] = keys
    return result


def _extract_class_name(raw: str) -> str:
    """Extract class name from a potential asset path.

    '/Game/AI/Tasks/AITask_ChaseTarget.AITask_ChaseTarget_C' → 'AITask_ChaseTarget_C'
    'BTTask_MoveTo' → 'BTTask_MoveTo'
    """
    if '.' in raw:
        return raw.rsplit('.', 1)[-1]
    return raw


def _token_to_node(tok: BTToken) -> dict[str, Any]:
    """Convert a single token to a node dict (no children yet)."""
    kind = tok.kind
    node: dict[str, Any] = {}

    if kind in ("root", "composite"):
        node["kind"] = "Composite"
        node["type"] = tok.group(0) if kind == "root" else tok.raw
    elif kind == "task":
        node["kind"] = "Task"
        node["class"] = _extract_class_name(tok.group(0))
        node["properties"] = _parse_bt_args(tok.group(1))
    elif kind == "decorator":
        node["kind"] = "Decorator"
        node["class"] = _extract_class_name(tok.group(0))
        props = _parse_bt_args(tok.group(1))
        if "FlowControl" in props:
            node["flow_control"] = props.pop("FlowControl")
        node["properties"] = props
    elif kind == "service":
        node["kind"] = "Service"
        node["class"] = _extract_class_name(tok.group(0))
        node["properties"] = _parse_bt_args(tok.group(1))
    elif kind == "run_subtree":
        node["kind"] = "RunBehaviorTree"
        node["subtree_path"] = tok.group(0)

    return node


def _build_tree_stack(tokens: list[BTToken]) -> dict[str, Any] | None:
    """Build tree using an indent-based stack."""
    root_node: dict[str, Any] | None = None
    stack: list[tuple[int, dict[str, Any]]] = []

    for tok in tokens:
        # Pop until we find the parent
        while stack and stack[-1][0] >= tok.indent:
            stack.pop()

        node = _token_to_node(tok)

        if not stack:
            # First token = root level — but if it's Root: Type,
            # we create a proper root node.
            if tok.kind == "root":
                root_node = node
            else:
                root_node = node
            root_node["kind"] = "Root" if tok.kind in ("root",) else root_node.get("kind", "Composite")
            stack.append((tok.indent, root_node))
            continue

        parent_indent, parent = stack[-1]

        # Attach to parent
        if tok.kind == "decorator":
            parent.setdefault("decorators", []).append(node)
        elif tok.kind == "service":
            parent.setdefault("services", []).append(node)
        else:
            parent.setdefault("children", []).append(node)

        stack.append((tok.indent, node))

    return root_node


# ── JSON → Text decompiler ──────────────────────────────────────────────────

def decompile_bt_json_to_text(data: dict[str, Any]) -> str:
    """Convert BehaviorTree JSON to indented text."""
    lines: list[str] = []

    name = data.get("name", "BT_New")
    lines.append(f"behaviortree {name}:")

    bb_name = data.get("blackboard", {}).get("name", "")
    bb_path = data.get("blackboard", {}).get("path", "")
    if bb_path:
        lines.append(f"    blackboard: {bb_path}")
    elif bb_name:
        lines.append(f"    blackboard: {bb_name}")

    # Keys
    keys = data.get("blackboard", {}).get("keys", [])
    for k in keys:
        inst = " [InstanceEditable]" if k.get("instance_editable") or k.get("is_instance_editable") else ""
        lines.append(f"        key {k['name']}: {_key_type_short(k.get('type', 'Object'))}{inst}")

    # Tree
    root = data.get("root", {})
    if root:
        _decompile_node(root, lines, indent=1)

    return "\n".join(lines)


def _key_type_short(type_name: str) -> str:
    """Convert UE BlackboardKeyType class name to short form."""
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
        if ue_name.lower() in type_name.lower():
            return short
    return type_name.replace("BlackboardKeyType_", "")


def _decompile_node(node: dict[str, Any], lines: list[str], indent: int) -> None:
    prefix = "    " * indent
    kind = node.get("kind", "Unknown")

    if kind in ("Root", "Composite"):
        node_type = node.get("type", "Selector")
        if kind == "Root":
            lines.append(f"{prefix}Root: {node_type}")
        else:
            lines.append(f"{prefix}{node_type}")

        # Decorators
        for dec in node.get("decorators", []):
            dec_class = dec.get("class", "UnknownDecorator")
            props = dec.get("properties", {})
            fc = dec.get("flow_control", "")
            if fc and fc != "None":
                props["FlowControl"] = fc
            args_str = _format_args(props)
            lines.append(f"{prefix}    Decorator: {dec_class}({args_str})" if args_str
                         else f"{prefix}    Decorator: {dec_class}")

        # Services
        for svc in node.get("services", []):
            svc_class = svc.get("class", "UnknownService")
            args_str = _format_args(svc.get("properties", {}))
            lines.append(f"{prefix}    Service: {svc_class}({args_str})" if args_str
                         else f"{prefix}    Service: {svc_class}")

        # Children
        for child in node.get("children", []):
            _decompile_node(child, lines, indent + 1)

    elif kind == "Task":
        task_class = node.get("class", "UnknownTask")
        props = node.get("properties", {})
        args_str = _format_args(props)
        lines.append(f"{prefix}Task: {task_class}({args_str})" if args_str
                     else f"{prefix}Task: {task_class}")

        for dec in node.get("decorators", []):
            dec_class = dec.get("class", "UnknownDecorator")
            dprops = dec.get("properties", {})
            dargs = _format_args(dprops)
            lines.append(f"{prefix}    Decorator: {dec_class}({dargs})" if dargs
                         else f"{prefix}    Decorator: {dec_class}")

    elif kind == "RunBehaviorTree":
        sp = node.get("subtree_path", "")
        lines.append(f"{prefix}RunBehaviorTree: {sp}")


def _format_args(props: dict[str, str]) -> str:
    if not props:
        return ""
    return ", ".join(f"{k}={v}" for k, v in props.items())


# ── Validator ───────────────────────────────────────────────────────────────

@dataclass
class BTValidationIssue:
    message: str
    line_no: int = 0
    severity: str = "error"  # error | warning


@dataclass
class BTValidationResult:
    issues: list[BTValidationIssue] = field(default_factory=list)

    @property
    def errors(self) -> list[BTValidationIssue]:
        return [i for i in self.issues if i.severity == "error"]

    @property
    def warnings(self) -> list[BTValidationIssue]:
        return [i for i in self.issues if i.severity == "warning"]

    @property
    def is_valid(self) -> bool:
        return len(self.errors) == 0


def validate_bt_text(text: str) -> BTValidationResult:
    """Validate indented behavior tree text."""
    result = BTValidationResult()

    try:
        tokens = tokenize_bt(text)
    except Exception as e:
        result.issues.append(BTValidationIssue(str(e), 0, "error"))
        return result

    if not tokens:
        result.issues.append(BTValidationIssue("Empty input", 0, "error"))
        return result

    if tokens[0].kind != "behaviortree":
        result.issues.append(BTValidationIssue(
            "Must start with 'behaviortree Name:'", tokens[0].line_no, "error"))
        return result

    has_root = any(t.kind == "root" for t in tokens)
    if not has_root:
        result.issues.append(BTValidationIssue(
            "Missing Root node (e.g., 'Root: Selector')", 0, "error"))

    has_task = any(t.kind == "task" for t in tokens)
    if not has_task:
        result.warnings.append(BTValidationIssue(
            "BehaviorTree has no Task nodes", 0, "warning"))

    # Check for empty composites
    for i, tok in enumerate(tokens):
        if tok.kind in ("composite",):
            # Check if there's a next token with deeper indent
            has_children = False
            for j in range(i + 1, min(i + 3, len(tokens))):
                if tokens[j].indent > tok.indent:
                    has_children = True
                    break
            if not has_children:
                result.warnings.append(BTValidationIssue(
                    f"Composite '{tok.raw}' has no children", tok.line_no, "warning"))

    return result


# ── Public API ──────────────────────────────────────────────────────────────

def _strip_bt_frontmatter(text: str) -> tuple[str, dict[str, str]]:
    """Strip YAML frontmatter (--- ... ---) from BT text."""
    meta: dict[str, str] = {}
    if text.startswith("---"):
        end = text.find("---", 3)
        if end != -1:
            fm = text[3:end].strip()
            for line in fm.split("\n"):
                if ":" in line:
                    k, v = line.split(":", 1)
                    meta[k.strip()] = v.strip()
            text = text[end + 3:].strip()
    return text, meta


def compile_bt_file(bt_path: str | Path) -> dict[str, Any]:
    """Read a .bttxt file (with optional frontmatter) and compile to BehaviorTree JSON."""
    with open(bt_path, "r", encoding="utf-8") as f:
        raw = f.read()
    text, _ = _strip_bt_frontmatter(raw)
    return compile_bt_text_to_json(text)


def decompile_bt_file(json_path: str | Path) -> str:
    """Read a BehaviorTree JSON file and decompile to indented text."""
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return decompile_bt_json_to_text(data)


def validate_bt_file(bt_path: str | Path) -> BTValidationResult:
    """Validate a .bttxt file."""
    with open(bt_path, "r", encoding="utf-8") as f:
        raw = f.read()
    text, _ = _strip_bt_frontmatter(raw)
    return validate_bt_text(text)
