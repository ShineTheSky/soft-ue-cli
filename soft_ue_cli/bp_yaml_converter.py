"""Convert between blueprint JSON and YAML with anchor references.

Nodes get YAML anchors (``&node_id``). Connections reference nodes by alias
(``*node_id``), making the graph structure directly readable.
"""

from __future__ import annotations

import datetime
from typing import Any

from ruamel.yaml import YAML
from ruamel.yaml.comments import CommentedMap, CommentedSeq


def _coerce_to_commented(obj: Any) -> Any:
    """Recursively convert plain dict/list to ruamel.yaml CommentedMap/CommentedSeq."""
    if isinstance(obj, dict):
        cm = CommentedMap()
        for k, v in obj.items():
            cm[k] = _coerce_to_commented(v)
        return cm
    if isinstance(obj, list):
        cs = CommentedSeq()
        for item in obj:
            cs.append(_coerce_to_commented(item))
        return cs
    return obj


# ── JSON → YAML ──────────────────────────────────────────────────────────────────

def json_to_yaml(blueprint_json: dict[str, Any]) -> str:
    """Convert a blueprint JSON dict to YAML with node anchors.

    Each node in the event graph and function/macro graphs gets a YAML anchor
    using its ``id``. Connections reference nodes via ``*anchor`` aliases.
    """
    data = _coerce_to_commented(blueprint_json)

    # Build a lookup of id → CommentedMap for all nodes across all graphs
    _set_graph_node_anchors(data.get("nodes", []), "event")

    for fg in data.get("function_graphs") or []:
        _set_graph_node_anchors(fg.get("nodes", []), str(fg.get("name", "function")))
    for mg in data.get("macro_graphs") or []:
        _set_graph_node_anchors(mg.get("nodes", []), str(mg.get("name", "macro")))

    # Replace connections: flat strings → anchored references
    _convert_connections_to_yaml(data)
    _annotate_cdo_defaults(data)

    yaml = YAML()
    yaml.indent(mapping=2, sequence=4, offset=2)
    yaml.width = 4096  # prevent line wrapping

    import io
    buf = io.StringIO()
    yaml.dump(data, buf)
    return buf.getvalue()


def _annotate_cdo_defaults(data: CommentedMap) -> None:
    """Render CDO category/display/source metadata as YAML comments."""
    defaults = data.get("defaults")
    if not isinstance(defaults, CommentedMap):
        return
    props = defaults.get("properties")
    if not isinstance(props, CommentedSeq):
        return

    last_category = None
    last_owner = None
    for idx, prop in enumerate(props):
        if not isinstance(prop, CommentedMap):
            continue
        category = str(prop.pop("category", "") or "Default")
        display_name = str(prop.pop("display_name", "") or "")
        owner_class = str(prop.pop("owner_class", "") or "")
        source_hint = str(prop.pop("source_hint", "") or "")

        before_lines: list[str] = []
        if category != last_category:
            before_lines.append(f"## CDO / {category}")
            last_category = category
            last_owner = None
        if owner_class and owner_class != last_owner:
            before_lines.append(f"@owner {owner_class}")
            last_owner = owner_class
        if display_name:
            before_lines.append(f"@display {display_name}")
        if source_hint:
            before_lines.append(f"@see {source_hint}")

        if before_lines:
            props.yaml_set_comment_before_after_key(idx, before="\n".join(before_lines))


def _anchor_name(graph_name: str, node_id: str) -> str:
    """Return a YAML-safe graph-local anchor name for a UE node id."""
    safe_graph = "".join(ch if ch.isalnum() or ch in "_-" else "_" for ch in graph_name)
    return f"{safe_graph}__{node_id}" if safe_graph else node_id


def _set_graph_node_anchors(nodes: list[Any], graph_name: str) -> None:
    """Set YAML anchors on nodes using graph scope to avoid duplicate ids."""
    for i, node in enumerate(nodes):
        if not isinstance(node, CommentedMap):
            continue
        node_id = node.get("id", "")
        if node_id:
            node.yaml_set_anchor(_anchor_name(graph_name, str(node_id)))


def _nodes_by_id(nodes: Any) -> dict[str, CommentedMap]:
    """Build a node lookup for a single graph."""
    node_map: dict[str, CommentedMap] = {}
    if not isinstance(nodes, (list, CommentedSeq)):
        return node_map
    for node in nodes:
        if isinstance(node, CommentedMap):
            node_id = node.get("id", "")
            if node_id:
                node_map[str(node_id)] = node
    return node_map


def _convert_connections_to_yaml(data: CommentedMap) -> None:
    """Replace flat connection strings with anchored object references."""
    _rewrite_conn_list(data.get("connections"), _nodes_by_id(data.get("nodes")), data)

    for fg in data.get("function_graphs") or []:
        _rewrite_conn_list(fg.get("connections"), _nodes_by_id(fg.get("nodes")), fg)
    for mg in data.get("macro_graphs") or []:
        _rewrite_conn_list(mg.get("connections"), _nodes_by_id(mg.get("nodes")), mg)


def _rewrite_conn_list(
    conns: Any, node_map: dict[str, CommentedMap], parent: Any
) -> None:
    """Rewrite a list of connection dicts to use anchored node refs."""
    if not isinstance(conns, (list, CommentedSeq)):
        return
    new_conns = CommentedSeq()
    for conn in conns:
        if not isinstance(conn, (dict, CommentedMap)):
            new_conns.append(conn)
            continue
        new_conn = CommentedMap()
        for key in ("from", "to"):
            flat = conn.get(key, "")
            if isinstance(flat, str) and "." in flat:
                node_id, pin_name = flat.split(".", 1)
                if node_id in node_map:
                    ref = CommentedMap()
                    ref["node"] = node_map[node_id]
                    ref["pin"] = pin_name
                    new_conn[key] = ref
                    continue
            new_conn[key] = conn.get(key)
        new_conns.append(new_conn)
    # Replace the connection list in parent
    target_list = parent
    if hasattr(parent, "get") and parent.get("connections") is conns:
        parent["connections"] = new_conns


# ── YAML → JSON ──────────────────────────────────────────────────────────────────

def yaml_to_json(yaml_text: str) -> dict[str, Any]:
    """Convert YAML with node anchors back to flat JSON for create-blueprint-from-json.

    Resolves ``*node_id`` aliases in connections back to ``"node_id.pin_name"`` strings.
    """
    yaml = YAML()
    data = yaml.load(yaml_text)

    # Collect all node id → guid mappings from resolved YAML
    # (After YAML load, aliases become references to the same object)
    node_guid_map: dict[int, str] = {}  # id(node) → id string

    _index_nodes_by_object(data.get("nodes", []), node_guid_map)
    for fg in data.get("function_graphs") or []:
        _index_nodes_by_object(fg.get("nodes", []), node_guid_map)
    for mg in data.get("macro_graphs") or []:
        _index_nodes_by_object(mg.get("nodes", []), node_guid_map)

    # Convert connections back to flat strings
    _convert_connections_to_json(data, node_guid_map)
    _normalize_defaults_for_bridge(data)

    return data


def _normalize_defaults_for_bridge(data: dict[str, Any]) -> None:
    """Convert human-facing CDO YAML fields back to bridge JSON fields."""
    defaults = data.get("defaults")
    if not isinstance(defaults, dict):
        return
    props = defaults.get("properties")
    if not isinstance(props, list):
        return
    for prop in props:
        if not isinstance(prop, dict):
            continue
        path = prop.get("path")
        if path and not prop.get("name"):
            prop["name"] = path
        for meta_key in ("path", "category", "display_name", "owner_class", "source_hint"):
            prop.pop(meta_key, None)


def _index_nodes_by_object(nodes: list[Any], obj_map: dict[int, str]) -> None:
    """Map Python object id → node's id string for alias resolution."""
    for node in nodes:
        if isinstance(node, dict):
            node_id = node.get("id", "")
            if node_id:
                obj_map[id(node)] = str(node_id)


def _convert_connections_to_json(data: dict[str, Any], obj_map: dict[int, str]) -> None:
    """Replace anchored connection refs with flat ``"node_id.pin"`` strings."""
    _rewrite_conn_list_to_json(data.get("connections"), obj_map)

    for fg in data.get("function_graphs") or []:
        _rewrite_conn_list_to_json(fg.get("connections"), obj_map)
    for mg in data.get("macro_graphs") or []:
        _rewrite_conn_list_to_json(mg.get("connections"), obj_map)


def _rewrite_conn_list_to_json(conns: Any, obj_map: dict[int, str]) -> None:
    """Rewrite anchored node references back to flat strings."""
    if not isinstance(conns, list):
        return
    for conn in conns:
        if not isinstance(conn, dict):
            continue
        for key in ("from", "to"):
            val = conn.get(key)
            if isinstance(val, dict) and "node" in val and "pin" in val:
                node_obj = val["node"]
                node_id = obj_map.get(id(node_obj), "")
                pin = val["pin"]
                if node_id:
                    conn[key] = f"{node_id}.{pin}"
