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
    # Build key definition lines
    key_lines: list[str] = []
    for k in keys:
        name = k.get("name", "")
        ktype = k.get("type", "")
        ue_cls = _TYPE_MAP.get(ktype, "")
        if not name or not ue_cls:
            continue
        key_lines.append(f'        ("{name}", "{ue_cls}"),')

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
    for _name, _type_cls in _key_defs:
        ktc = unreal.load_class(None, "/Script/AIModule." + _type_cls)
        if not ktc:
            print("WARNING: class not found:", _type_cls)
            continue
        entry = unreal.BlackboardEntry()
        entry.set_editor_property("EntryName", _name)
        kt = unreal.new_object(ktc, bb)
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
