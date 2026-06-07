"""Convert query-blueprint + query-blueprint-graph output into create-blueprint-from-json input.

The unified JSON includes a _header section for human-readable metadata and is
directly compatible with ``create-blueprint-from-json``.
"""

from __future__ import annotations

import datetime
from typing import Any


def convert_to_create_json(
    bp_json: dict[str, Any],
    graph_json: dict[str, Any] | None = None,
    *,
    asset_path: str = "",
    description: str = "",
) -> dict[str, Any]:
    """Merge query-blueprint and query-blueprint-graph results into create-ready JSON.

    Parameters
    ----------
    bp_json:
        Result from ``query-blueprint`` (contains variables, defaults, parent_class, etc.)
    graph_json:
        Optional result from ``query-blueprint-graph`` (graphs with nodes/pins/connections).
        When absent, the output contains only variables + defaults (data-only blueprint).
    asset_path:
        Override the asset path in the output. Defaults to ``bp_json["path"]``.
    description:
        Human-readable description for the _header.

    Returns
    -------
    dict ready for ``create-blueprint-from-json``, with added ``_header`` metadata.
    """
    source_path = bp_json.get("path", asset_path)
    parent_class = bp_json.get("parent_class", "Actor")
    bp_type = bp_json.get("blueprint_type", "Normal")

    # ── Header ──────────────────────────────────────────────────────────────────
    header: dict[str, str] = {
        "description": description or f"Blueprint export of {bp_json.get('name', source_path)}",
        "created": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source_asset": source_path,
        "parent_class": parent_class,
        "blueprint_type": bp_type,
    }

    # ── Variables ───────────────────────────────────────────────────────────────
    variables: list[dict[str, Any]] = []
    var_items = bp_json.get("variables", {}).get("items", [])
    for v in var_items:
        entry: dict[str, Any] = {
            "name": v.get("name", ""),
            "type": v.get("type", ""),
        }
        if v.get("is_array"):
            entry["is_array"] = True
        if v.get("sub_type"):
            entry["sub_type"] = v["sub_type"]
        if v.get("category"):
            entry["category"] = v.get("category", "")
        flags = v.get("flags", [])
        if flags:
            entry["flags"] = flags
        variables.append(entry)

    # ── SCS Components ──────────────────────────────────────────────────────────
    # Build a lookup of component_name → {property_name: value} from component_overrides
    comp_overrides_lookup: dict[str, dict[str, str]] = {}
    for co in bp_json.get("component_overrides", {}).get("components", []):
        co_name = co.get("name", "")
        if not co_name:
            continue
        props: dict[str, str] = {}
        for ov in co.get("overrides", []):
            pname = ov.get("name", "")
            pval = ov.get("value", "")
            if pname and pval:
                props[pname] = pval
        if props:
            comp_overrides_lookup[co_name] = props

    components: list[dict[str, Any]] = []
    comp_items = bp_json.get("components", {}).get("items", [])
    for c in comp_items:
        entry: dict[str, Any] = {"name": c.get("name", ""), "class": c.get("class", "")}
        if c.get("parent"):
            entry["parent"] = c["parent"]
        # Merge component template properties from component_overrides
        comp_name = c.get("name", "")
        if comp_name in comp_overrides_lookup:
            entry["properties"] = comp_overrides_lookup[comp_name]
        components.append(entry)

    # ── CDO Defaults ────────────────────────────────────────────────────────────
    defaults_section: dict[str, Any] = {}
    bp_defaults = bp_json.get("defaults", {}).get("properties", [])
    if bp_defaults:
        # Skip runtime-only properties
        _RUNTIME = frozenset({
            "UberGraphFrame", "bIsActive", "bIsAbilityEnding", "bIsCancelable",
            "bIsBlockingOtherAbilities", "bMarkPendingKillOnAbilityEnd",
            "CurrentActivationInfo", "CurrentEventData", "ActiveTasks",
            "CurrentMontage", "RemoteInstanceEnded", "bReplicateInputDirectly",
        })
        meaningful = []
        for p in bp_defaults:
            name = p.get("path") or p.get("name", "")
            if name in _RUNTIME:
                continue
            dv = p.get("default_value", "")
            if dv in ("None", "0", "0.000000", "", "(GameplayTags=)",
                       '(TagName="")', "ReplicateNo", "InstancedPerExecution",
                       "LocalPredicted", "ClientOrServer"):
                continue
            entry = {"path": name, "default_value": dv}
            for meta_key in ("category", "display_name", "owner_class", "source_hint"):
                if p.get(meta_key):
                    entry[meta_key] = p[meta_key]
            meaningful.append(entry)
        if meaningful:
            defaults_section["properties"] = meaningful

    # ── Nodes + Connections (all graph types) ───────────────────────────────────
    json_nodes: list[dict[str, Any]] = []
    connections: list[dict[str, str]] = []
    function_graphs: list[dict[str, Any]] = []
    macro_graphs: list[dict[str, Any]] = []

    if graph_json:
        for graph in graph_json.get("graphs", []):
            graph_type = graph.get("type", "")
            graph_name = graph.get("name", "")

            # Skip ConstructionScript (auto-generated, not meaningful)
            if graph_type == "construction_script":
                continue

            g_nodes, g_connections = _process_graph_nodes(graph)

            if graph_type == "event":
                json_nodes = g_nodes
                connections = g_connections
            elif graph_type == "function":
                function_graphs.append({
                    "name": graph_name,
                    "nodes": g_nodes,
                    "connections": g_connections,
                })
            elif graph_type == "macro":
                macro_graphs.append({
                    "name": graph_name,
                    "nodes": g_nodes,
                    "connections": g_connections,
                })

    # ── Assemble ─────────────────────────────────────────────────────────────────
    result: dict[str, Any] = {
        "_header": header,
        "parent_class": parent_class,
        "asset_path": asset_path or source_path,
        "variables": variables,
        "nodes": json_nodes,
        "connections": connections,
    }
    if function_graphs:
        result["function_graphs"] = function_graphs
    if macro_graphs:
        result["macro_graphs"] = macro_graphs
    if defaults_section:
        result["defaults"] = defaults_section
    if components:
        result["components"] = components

    # ── Timelines ──────────────────────────────────────────────────────────────
    timelines = bp_json.get("timelines", {}).get("items", [])
    if timelines:
        result["timelines"] = timelines

    return result


def _process_graph_nodes(graph: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    """Process a single graph dict into (nodes, connections) ready for JSON."""
    json_nodes: list[dict[str, Any]] = []
    connections: list[dict[str, str]] = []
    seen_connections: set[tuple[str, str, str, str]] = set()

    for node in graph.get("nodes", []):
        guid = node.get("guid", "")
        node_class = node.get("class", "")
        node_def: dict[str, Any] = {"id": guid, "type": node_class}

        # ── Type-specific fields ──
        if "K2Node_Event" in node_class or "K2Node_ComponentBoundEvent" in node_class or "K2Node_CustomEvent" in node_class \
           or "K2Node_FunctionEntry" in node_class or "K2Node_FunctionResult" in node_class:
            evt = node.get("event_name", "")
            if evt:
                node_def["event"] = evt
            elif "K2Node_FunctionEntry" in node_class or "K2Node_FunctionResult" in node_class:
                evt = node.get("title", "")
                if evt:
                    node_def["event"] = evt

        # ComponentBoundEvent: pass through component/delegate references
        cname = node.get("component_name", "")
        dname = node.get("delegate_name", "")
        doclass = node.get("delegate_owner_class", "")
        if cname:
            node_def["component_name"] = cname
        if dname:
            node_def["delegate_name"] = dname
        if doclass:
            node_def["delegate_owner_class"] = doclass

        # Field-driven: any node that queries back function_name gets it
        func = node.get("function_name", "")
        if func:
            node_def["function"] = func
        fc = node.get("function_class_path", "") or node.get("function_class", "")
        if fc and "Latent" not in node_class:
            node_def["function_class_path"] = fc
        # Latent nodes only: carry ProxyFactory metadata as properties
        if "Latent" in node_class:
            if fc:
                node_def.setdefault("properties", {})["ProxyFactoryClass"] = fc
            pc = node.get("proxy_class_path", "")
            if pc:
                node_def.setdefault("properties", {})["ProxyClass"] = pc
            if func:
                node_def.setdefault("properties", {})["ProxyFactoryFunctionName"] = func

        # PromotableOperator: carry operation_name, num_additional_inputs
        oname = node.get("operation_name", "")
        nai = node.get("num_additional_inputs")
        if oname or nai is not None:
            promo_props = {}
            if oname:
                promo_props["OperationName"] = oname
            if nai is not None:
                promo_props["NumAdditionalInputs"] = str(nai)
            node_def.setdefault("properties", {}).update(promo_props)

        if "K2Node_DynamicCast" in node_class:
            cast_to = node.get("cast_to", "")
            if not cast_to:
                title = node.get("title", "")
                cast_to = title.replace("Cast To ", "").strip()
            if cast_to:
                node_def["cast_to"] = cast_to

        if "VariableGet" in node_class or "K2Node_VariableSet" in node_class:
            var_name = node.get("variable_name", "") or _extract_variable_name(node)
            if var_name:
                node_def["variable"] = var_name
                parent_cls = node.get("variable_parent_class", "")
                if parent_cls:
                    node_def["variable_parent_class"] = parent_cls

        # Timeline: pass through timeline_name
        tname = node.get("timeline_name", "")
        if tname:
            node_def["timeline_name"] = tname

        # BreakStruct: pass through struct_type
        stype = node.get("struct_type", "")
        if stype:
            node_def["struct_type"] = stype

        # MacroInstance: carry macro graph reference as properties
        mb = node.get("macro_blueprint", "")
        mg = node.get("macro_graph", "")
        if mb or mg:
            macro_props = {}
            if mb:
                macro_props["MacroGraphReference.GraphBlueprint"] = mb
            if mg:
                macro_props["MacroGraphReference.MacroGraph"] = mg
            node_def.setdefault("properties", {}).update(macro_props)

        # MakeArray: carry NumInputs as property
        ni = node.get("num_inputs")
        if ni is not None:
            node_def.setdefault("properties", {})["NumInputs"] = str(ni)

        # ── Position ──
        pos = node.get("position")
        if pos and isinstance(pos, dict):
            node_def["position"] = [pos.get("x", 0), pos.get("y", 0)]

        # ── Pin defaults + types ──
        pin_defaults: dict[str, str] = {}
        pin_default_objects: dict[str, str] = {}
        pin_types: dict[str, str] = {}
        for pin in node.get("pins", []):
            pin_name = pin.get("name", "")
            dv = pin.get("default_value", "")
            if dv and dv not in ("None", "0.0", "0", "false", ""):
                pin_defaults[pin_name] = dv
            dob = pin.get("default_object", "")
            if dob:
                pin_default_objects[pin_name] = dob
            cat = pin.get("category", "")
            sub = pin.get("sub_category_object", "")
            if sub:
                pin_types[pin_name] = f"{cat}/{sub}"
            elif cat == "real":
                pin_types[pin_name] = "real/float"
            elif cat and cat not in ("exec",):
                pin_types[pin_name] = cat

            # ── Connections ──
            for conn in pin.get("connections", []):
                target_guid = conn.get("node_guid", "")
                target_pin = conn.get("pin_name", "")
                if not target_guid or not target_pin:
                    continue
                if pin.get("direction") == "output":
                    key = (guid, pin_name, target_guid, target_pin)
                    if key not in seen_connections:
                        seen_connections.add(key)
                        connections.append({"from": f"{guid}.{pin_name}", "to": f"{target_guid}.{target_pin}"})

        if pin_defaults:
            node_def["defaults"] = pin_defaults
        if pin_default_objects:
            node_def["default_objects"] = pin_default_objects
        if pin_types:
            node_def["pin_types"] = pin_types

        json_nodes.append(node_def)

    return json_nodes, connections


def _extract_variable_name(node: dict[str, Any]) -> str:
    """Extract the variable name from a VariableGet/VariableSet node."""
    # Try the node title first: "Get Xxx" or "Set Xxx"
    title = node.get("title", "")
    for prefix in ("Get ", "Set "):
        if title.startswith(prefix):
            return title[len(prefix):].strip()

    # Fallback: look for the variable output/input pin
    for pin in node.get("pins", []):
        name = pin.get("name", "")
        # Skip exec, delegate, self pins
        if name in ("execute", "then", "OutputDelegate", "Output_Get", "self"):
            continue
        cat = pin.get("category", "")
        if cat and cat not in ("exec", "delegate"):
            # The typed output/input pin usually has the variable type as name
            return name

    return ""
