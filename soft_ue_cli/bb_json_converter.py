"""BlackboardData JSON round-trip: read blackboard as JSON, create from JSON.

Uses ``query-asset`` + ``create-asset`` + ``run-python-script`` — zero C++ changes.
"""

from __future__ import annotations

import json
import re
import textwrap
from typing import Any

# Key type string → UE class short name (used in load_class paths)
_TYPE_MAP: dict[str, str] = {
    "Bool": "BlackboardKeyType_Bool",
    "Int": "BlackboardKeyType_Int",
    "Float": "BlackboardKeyType_Float",
    "String": "BlackboardKeyType_String",
    "Name": "BlackboardKeyType_Name",
    "Vector": "BlackboardKeyType_Vector",
    "Rotator": "BlackboardKeyType_Rotator",
    "Object": "BlackboardKeyType_Object",
    "Class": "BlackboardKeyType_Class",
    "Enum": "BlackboardKeyType_Enum",
}


def parse_keys_text(raw: str) -> list[dict[str, Any]]:
    """Parse UE text export format for TArray<FBlackboardEntry> into clean list.

    Input example:
        ((EntryName="SelfActor",KeyType="...BlackboardKeyType_Object'..."),(...))

    Returns:
        [{"name": "SelfActor", "type": "Object"}, ...]
    """
    if not raw or raw.strip() in ("()", ""):
        return []

    pattern = r'EntryName="([^"]+)",KeyType="/Script/AIModule\.BlackboardKeyType_([A-Za-z]+)\''
    entries: list[dict[str, Any]] = []
    for match in re.finditer(pattern, raw):
        entries.append({
            "name": match.group(1),
            "type": match.group(2),
        })
    return entries


def query_blackboard(asset_path: str) -> dict[str, Any]:
    """Read a BlackboardData asset and return clean JSON."""
    from .__main__ import _run_tool

    script = textwrap.dedent(f"""\
    import json
    import unreal

    bb = unreal.load_asset({asset_path!r})
    if not bb:
        raise RuntimeError("BlackboardData not found: {asset_path}")

    parent = bb.get_editor_property("Parent")
    keys = []
    for entry in bb.get_editor_property("Keys"):
        key_type = entry.get_editor_property("KeyType")
        if not key_type:
            continue
        cls_name = key_type.get_class().get_name()
        short_type = cls_name.replace("BlackboardKeyType_", "")
        item = {{
            "name": str(entry.get_editor_property("EntryName")),
            "type": short_type,
        }}
        if cls_name == "BlackboardKeyType_Enum":
            enum_type = key_type.get_editor_property("EnumType")
            enum_name = key_type.get_editor_property("EnumName")
            if enum_type:
                item["enum_path"] = enum_type.get_path_name()
            if enum_name:
                item["enum_name"] = enum_name
        keys.append(item)

    payload = {{
        "name": bb.get_name(),
        "path": {asset_path!r},
        "parent": parent.get_path_name() if parent else None,
        "keys": keys,
        "key_count": len(keys),
    }}
    print("__BB_JSON__" + json.dumps(payload, ensure_ascii=False))
    """)

    py_result = _run_tool("run-python-script", {"script": script})
    output = py_result.get("output", "") if isinstance(py_result, dict) else ""
    for line in reversed(output.splitlines()):
        line = line.strip()
        if line.startswith("__BB_JSON__"):
            return json.loads(line[len("__BB_JSON__"):])

    # Fallback to the older asset-text parser if Python output was unavailable.
    result = _run_tool("query-asset", {"asset_path": asset_path})

    name = result.get("name", "")
    path = asset_path

    # Extract parent path
    parent = None
    for prop in result.get("properties", []):
        if prop.get("name") == "Parent" and prop.get("value", "None") != "None":
            parent = prop["value"]
            break

    # Parse Keys
    keys_raw = ""
    for prop in result.get("properties", []):
        if prop.get("name") == "Keys":
            keys_raw = prop.get("value", "")
            break

    keys = parse_keys_text(keys_raw)

    return {
        "name": name,
        "path": path,
        "parent": parent,
        "keys": keys,
        "key_count": len(keys),
    }


def _build_keys_script(
    asset_path: str,
    parent: str | None,
    keys: list[dict[str, Any]],
) -> str:
    """Generate a UE Python script that populates keys on an EXISTING BlackboardData.

    The asset must already exist (created via ``create-asset``). This script
    loads it, creates key type subobjects, and saves.
    """
    # Build key definition lines. Enum keys may include enum_path/enum_type/enum
    # for Blueprint/UserDefinedEnum assets, or enum_name for native enums.
    key_lines: list[str] = []
    for k in keys:
        name = k.get("name", "")
        ktype = k.get("type", "")
        ue_cls = _TYPE_MAP.get(ktype, "")
        if not name or not ue_cls:
            continue
        enum_path = (
            k.get("enum_path")
            or k.get("enum_type")
            or k.get("enum")
            or k.get("sub_type")
            or ""
        )
        enum_name = k.get("enum_name") or k.get("native_enum_name") or ""
        key_lines.append(
            f'        {{"name": {name!r}, "type_cls": {ue_cls!r}, '
            f'"enum_path": {enum_path!r}, "enum_name": {enum_name!r}}},'
        )

    # Parent setter
    parent_block = ""
    if parent:
        parent_block = textwrap.dedent(f"""\

            # Set parent blackboard
            _p = unreal.load_asset("{parent}")
            if _p:
                bb.set_editor_property("Parent", _p)
            else:
                print("WARNING: parent asset not found: {parent}")
        """)

    script = textwrap.dedent(f"""\
    import unreal

    _key_defs = [
    {chr(10).join(key_lines)}
    ]

    bb = unreal.load_asset("{asset_path}")
    if not bb:
        raise RuntimeError("Asset not found: {asset_path}")
    print("Loaded:", bb.get_name())
    {textwrap.indent(parent_block.strip(), "    ").strip()}

    entries = []
    for _def in _key_defs:
        _name = _def.get("name", "")
        _type_cls = _def.get("type_cls", "")
        ktc = unreal.load_class(None, "/Script/AIModule." + _type_cls)
        if not ktc:
            print("WARNING: class not found:", _type_cls)
            continue
        entry = unreal.BlackboardEntry()
        entry.set_editor_property("EntryName", _name)
        kt = unreal.new_object(ktc, bb)
        if _type_cls == "BlackboardKeyType_Enum":
            _enum_path = _def.get("enum_path", "")
            _enum_name = _def.get("enum_name", "")
            if _enum_path:
                _enum = unreal.load_asset(_enum_path)
                if _enum:
                    kt.set_editor_property("EnumType", _enum)
                else:
                    print("WARNING: enum asset not found for key", _name, ":", _enum_path)
            if _enum_name:
                kt.set_editor_property("EnumName", _enum_name)
        entry.set_editor_property("KeyType", kt)
        entries.append(entry)

    bb.set_editor_property("Keys", entries)
    unreal.EditorAssetLibrary.save_loaded_asset(bb)
    print("OK: {asset_path} keys=" + str(len(entries)))
    """)
    return script


def build_create_script(
    json_data: dict[str, Any],
    asset_path: str | None = None,
) -> str:
    """Build the Python script to populate keys on an existing BlackboardData.

    Returns a Python script string for ``run-python-script``.
    """
    target_path = asset_path or json_data.get("path", json_data.get("asset_path", ""))
    if not target_path:
        raise ValueError("asset_path is required")

    parent = json_data.get("parent")
    keys = json_data.get("keys", [])

    return _build_keys_script(target_path, parent, keys)
