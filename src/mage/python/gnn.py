# Copyright 2026 Memgraph Ltd.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.txt; by using this
# file, you agree to be bound by the terms of the Business
# Source License, and you may not use this file except in
# compliance with the Business Source License.
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0, included in the file
# licenses/APL.txt.

"""
GNN integration module for Memgraph.

Provides export/import procedures for PyTorch Geometric (PyG) and
TensorFlow GNN (TF-GNN) formats. All exports produce a single JSON
string that can be deserialized on the client side and fed into the
respective framework.

Example Cypher usage::

    CALL gnn.pyg_export(['feat'], ['weight'], 'class')
    YIELD json_data;

    CALL gnn.pyg_import($json_data, 'Node', 'EDGE')
    YIELD nodes_created, edges_created;

    CALL gnn.tf_export(['score'], ['weight'])
    YIELD json_data;

    CALL gnn.tf_import($json_data, 'Node', 'EDGE')
    YIELD nodes_created, edges_created;
"""

import json
from typing import Any, Dict, List, Optional, Tuple

import mgp

# ---- shared helpers ------------------------------------------------


def _collect_vertices_and_edges(
    ctx: mgp.ProcCtx,
) -> Tuple[List[mgp.Vertex], List[mgp.Edge]]:
    vertices = list(ctx.graph.vertices)
    edges = []
    for vertex in vertices:
        for edge in vertex.out_edges:
            edges.append(edge)
    return vertices, edges


def _property_to_numeric(value: Any) -> Optional[float]:
    if value is None:
        return None
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value)
        except ValueError:
            return None
    return None


def _is_numeric_list(value: Any) -> bool:
    if not isinstance(value, (list, tuple)):
        return False
    return all(isinstance(v, (int, float)) for v in value)


def _extract_features_with_spec(
    items: List[Any],
    property_names: List[str],
) -> Tuple[List[List[float]], List[Dict[str, Any]]]:
    spec: List[Dict[str, Any]] = []
    for name in property_names:
        present = [item.properties.get(name, None) for item in items if item.properties.get(name, None) is not None]
        has_list = any(isinstance(val, (list, tuple)) for val in present)
        if has_list:
            if not all(_is_numeric_list(val) for val in present):
                raise ValueError(f"Property {name!r} mixes list and non-numeric-list values across elements.")
            widths = {len(val) for val in present}
            if len(widths) > 1:
                raise ValueError(f"Vector property {name!r} has inconsistent widths {sorted(widths)} across elements.")
            width = next(iter(widths)) if widths else 0
            is_int = bool(present) and all(
                isinstance(elem, int) and not isinstance(elem, bool) for val in present for elem in val
            )
            spec.append({"name": name, "width": width, "is_list": True, "is_int": is_int})
        else:
            is_int = bool(present) and all(isinstance(val, int) and not isinstance(val, bool) for val in present)
            spec.append({"name": name, "width": 1, "is_list": False, "is_int": is_int})

    features: List[List[float]] = []
    for item in items:
        row: List[float] = []
        props = item.properties
        for entry in spec:
            name = entry["name"]
            width = entry["width"]
            value = props.get(name, None)
            if entry["is_list"]:
                if value is None:
                    row.extend([0.0] * width)
                else:
                    row.extend(float(v) for v in value)
            else:
                num = _property_to_numeric(value)
                row.append(num if num is not None else 0.0)
        features.append(row)
    return features, spec


def _extract_features(
    items: List[Any],
    property_names: List[str],
) -> List[List[float]]:
    features, _ = _extract_features_with_spec(items, property_names)
    return features


def _collect_feature_values(
    items: List[Any],
    property_names: List[str],
) -> Dict[str, List[Any]]:
    features: Dict[str, List[Any]] = {n: [] for n in property_names}
    for item in items:
        props = item.properties
        for name in property_names:
            features[name].append(props.get(name, None))
    return features


# ---- PyG helpers ---------------------------------------------------


def _resolve_named_props(
    pyg_dict: Dict[str, Any],
    property_names: List[str],
    idx: int,
) -> Dict[str, Any]:
    """Pick property values from top-level JSON keys for a given index."""
    props: Dict[str, Any] = {}
    for name in property_names:
        if name in pyg_dict:
            vals = pyg_dict[name]
            if isinstance(vals, list) and idx < len(vals):
                props[name] = vals[idx]
    return props


def _coerce_spec_number(value: Any, is_int: bool) -> Any:
    if is_int and isinstance(value, float) and value.is_integer():
        return int(value)
    return value


def _resolve_feature_fallback(
    props: Dict[str, Any],
    feats: List[Any],
    property_names: Optional[List[str]],
    feature_spec: Optional[List[Dict[str, Any]]] = None,
) -> Dict[str, Any]:
    """Fill property values from a feature vector respecting recorded field widths."""
    if feature_spec:
        wanted = set(property_names) if property_names is not None else None
        col_idx = 0
        for entry in feature_spec:
            name = entry.get("name", "")
            width = int(entry.get("width", 1))
            is_list = bool(entry.get("is_list", False))
            is_int = bool(entry.get("is_int", False))
            if (wanted is None or name in wanted) and name not in props and col_idx + width <= len(feats):
                if is_list:
                    props[name] = [_coerce_spec_number(v, is_int) for v in feats[col_idx : col_idx + width]]
                elif width == 1:
                    props[name] = _coerce_spec_number(feats[col_idx], is_int)
            col_idx += width
        return props

    if property_names:
        if len(property_names) == 1 and len(feats) > 1:
            only_name = property_names[0]
            if only_name not in props:
                props[only_name] = list(feats)
            return props
        unresolved = [name for name in property_names if name not in props]
        if unresolved and len(feats) != len(property_names):
            raise ValueError(
                f"Feature vector length ({len(feats)}) does not match property_names ({len(property_names)}) without feature_spec."
            )
        for col_idx, name in enumerate(property_names):
            if name not in props and col_idx < len(feats):
                props[name] = feats[col_idx]
    elif not props:
        props["features"] = feats
    return props


def _pyg_export_dict(
    vertices: List[Any],
    edges: List[Any],
    node_property_names: Optional[List[str]],
    edge_property_names: Optional[List[str]],
    node_label_property: Optional[str],
) -> Dict[str, Any]:
    id_to_idx: Dict[int, int] = {}
    for idx, v in enumerate(vertices):
        id_to_idx[v.id] = idx

    src_indices: List[int] = []
    dst_indices: List[int] = []
    included_edges: List[Any] = []
    for edge in edges:
        s = edge.from_vertex.id
        d = edge.to_vertex.id
        if s in id_to_idx and d in id_to_idx:
            src_indices.append(id_to_idx[s])
            dst_indices.append(id_to_idx[d])
            included_edges.append(edge)

    result: Dict[str, Any] = {
        "edge_index": [src_indices, dst_indices],
        "num_nodes": len(vertices),
    }

    if node_property_names:
        x_feats, node_spec = _extract_features_with_spec(vertices, node_property_names)
        result["x"] = x_feats
        result["node_feature_spec"] = node_spec

    if edge_property_names:
        edge_feats, edge_spec = _extract_features_with_spec(included_edges, edge_property_names)
        result["edge_attr"] = edge_feats
        result["edge_feature_spec"] = edge_spec

    if node_label_property:
        result["y"] = [v.properties.get(node_label_property, None) for v in vertices]

    result["node_id_mapping"] = {str(k): v for k, v in id_to_idx.items()}
    result["idx_to_node_id"] = {str(v): k for k, v in id_to_idx.items()}
    result["labels"] = [[lb.name for lb in v.labels] for v in vertices]
    result["edge_types"] = [e.type.name for e in included_edges]
    return result


def _pyg_import_data(
    pyg_dict: Dict[str, Any],
    default_node_label: str,
    default_edge_type: str,
    node_property_names: Optional[List[str]],
    edge_property_names: Optional[List[str]],
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    edge_index = pyg_dict.get("edge_index", [[], []])
    num_nodes = pyg_dict.get("num_nodes", 0)
    x = pyg_dict.get("x")
    edge_attr = pyg_dict.get("edge_attr")
    node_feature_spec = pyg_dict.get("node_feature_spec")
    edge_feature_spec = pyg_dict.get("edge_feature_spec")
    y = pyg_dict.get("y")
    labels = pyg_dict.get("labels")
    edge_types = pyg_dict.get("edge_types")
    idx_to_node_id = pyg_dict.get("idx_to_node_id")

    nodes_data: List[Dict[str, Any]] = []
    for idx in range(num_nodes):
        nd: Dict[str, Any] = {"idx": idx}
        if labels and idx < len(labels) and labels[idx]:
            nd["labels"] = labels[idx]
        else:
            nd["labels"] = [default_node_label]

        props: Dict[str, Any] = {}
        if node_property_names:
            props = _resolve_named_props(pyg_dict, node_property_names, idx)
        if x is not None and idx < len(x):
            props = _resolve_feature_fallback(props, x[idx], node_property_names, node_feature_spec)
        if y is not None and idx < len(y) and y[idx] is not None:
            props["y"] = y[idx]
        if idx_to_node_id:
            orig_id = idx_to_node_id.get(str(idx))
            if orig_id is not None:
                props["_original_id"] = orig_id

        nd["properties"] = props
        nodes_data.append(nd)

    src = edge_index[0] if edge_index else []
    dst = edge_index[1] if len(edge_index) > 1 else []
    if len(src) != len(dst):
        raise ValueError(f"PyG edge_index source ({len(src)}) and target ({len(dst)}) lengths differ.")
    edges_data: List[Dict[str, Any]] = []
    for ei, (si, di) in enumerate(zip(src, dst)):
        ed: Dict[str, Any] = {
            "source_idx": si,
            "target_idx": di,
            "type": (edge_types[ei] if edge_types and ei < len(edge_types) else default_edge_type),
        }
        props = {}
        if edge_property_names:
            props = _resolve_named_props(pyg_dict, edge_property_names, ei)
        if edge_attr is not None and ei < len(edge_attr):
            props = _resolve_feature_fallback(props, edge_attr[ei], edge_property_names, edge_feature_spec)

        ed["properties"] = props
        edges_data.append(ed)

    return nodes_data, edges_data


# ---- TF-GNN helpers -----------------------------------------------


def _tf_infer_dtype(values: List[Any]) -> str:
    has_string = has_float = has_int = has_bool = False
    for value in values:
        if value is None:
            continue
        if isinstance(value, bool):
            has_bool = True
        elif isinstance(value, int):
            has_int = True
        elif isinstance(value, float):
            has_float = True
        elif isinstance(value, str):
            has_string = True
        elif isinstance(value, (list, tuple)):
            for item in value:
                if item is None:
                    continue
                if isinstance(item, bool):
                    has_bool = True
                elif isinstance(item, int):
                    has_int = True
                elif isinstance(item, float):
                    has_float = True
                elif isinstance(item, str):
                    has_string = True
        else:
            has_string = True
    if has_string:
        return "DT_STRING"
    if has_float:
        return "DT_FLOAT"
    if has_bool and not has_int:
        return "DT_BOOL"
    if has_int or has_bool:
        return "DT_INT64"
    return "DT_STRING"


def _tf_infer_shape(values: List[Any]) -> List[int]:
    lengths: List[int] = []
    for value in values:
        if isinstance(value, (list, tuple)):
            if any(isinstance(i, (list, tuple)) for i in value):
                return [-1]
            lengths.append(len(value))
    if not lengths:
        return []
    if all(ln == lengths[0] for ln in lengths):
        return [lengths[0]]
    return [-1]


def _tf_build_feature_schema(
    features: Dict[str, List[Any]],
) -> Dict[str, Dict[str, Any]]:
    return {
        name: {
            "dtype": _tf_infer_dtype(vals),
            "shape": _tf_infer_shape(vals),
        }
        for name, vals in features.items()
    }


def _tf_export_dict(
    vertices: List[Any],
    edges: List[Any],
    node_property_names: Optional[List[str]],
    edge_property_names: Optional[List[str]],
    node_set_name: str,
    edge_set_name: str,
) -> Dict[str, Any]:
    id_to_idx = {v.id: idx for idx, v in enumerate(vertices)}

    src_indices: List[int] = []
    dst_indices: List[int] = []
    included_edges: List[Any] = []
    for edge in edges:
        s = edge.from_vertex.id
        d = edge.to_vertex.id
        if s in id_to_idx and d in id_to_idx:
            src_indices.append(id_to_idx[s])
            dst_indices.append(id_to_idx[d])
            included_edges.append(edge)

    node_features: Dict[str, List[Any]] = {}
    edge_features: Dict[str, List[Any]] = {}
    if node_property_names:
        node_features = _collect_feature_values(vertices, node_property_names)
    if edge_property_names:
        edge_features = _collect_feature_values(included_edges, edge_property_names)

    schema = {
        "node_sets": {
            node_set_name: {
                "features": _tf_build_feature_schema(node_features),
            },
        },
        "edge_sets": {
            edge_set_name: {
                "source": node_set_name,
                "target": node_set_name,
                "features": _tf_build_feature_schema(edge_features),
            },
        },
        "context": {"features": {}},
    }

    graph = {
        "node_sets": {
            node_set_name: {
                "features": node_features,
                "sizes": [len(vertices)],
            },
        },
        "edge_sets": {
            edge_set_name: {
                "features": edge_features,
                "sizes": [len(src_indices)],
                "adjacency": {
                    "source": {
                        "node_set_name": node_set_name,
                        "indices": src_indices,
                    },
                    "target": {
                        "node_set_name": node_set_name,
                        "indices": dst_indices,
                    },
                },
            },
        },
        "context": {"features": {}},
    }

    return {"schema": schema, "graph": graph}


def _sum_component_sizes(sizes: List[Any], label: str) -> int:
    total = 0
    for component_size in sizes:
        if isinstance(component_size, bool) or not isinstance(component_size, int) or component_size < 0:
            raise ValueError(f"TF-GNN {label} component size must be a non-negative integer, got {component_size!r}.")
        total += component_size
    return total


def _tf_import_data(
    tfgnn_dict: Dict[str, Any],
    default_node_label: str,
    default_edge_type: str,
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    graph = tfgnn_dict.get("graph", tfgnn_dict)
    node_sets = graph.get("node_sets", {})
    edge_sets = graph.get("edge_sets", {})

    nodes_data: List[Dict[str, Any]] = []
    for ns_name, ns_data in node_sets.items():
        sizes = ns_data.get("sizes", [])
        size = _sum_component_sizes(sizes, f"node set {ns_name!r}") if sizes else 0
        feats = ns_data.get("features", {})
        if size == 0 and not sizes and feats:
            size = max(
                (len(v) for v in feats.values()),
                default=0,
            )
        for idx in range(size):
            props: Dict[str, Any] = {}
            for fname, vals in feats.items():
                if idx < len(vals) and vals[idx] is not None:
                    props[fname] = vals[idx]
            nodes_data.append(
                {
                    "idx": idx,
                    "node_set": ns_name,
                    "labels": [ns_name or default_node_label],
                    "properties": props,
                }
            )

    edges_data: List[Dict[str, Any]] = []
    for es_name, es_data in edge_sets.items():
        adj = es_data.get("adjacency", {})
        src_info = adj.get("source", {})
        dst_info = adj.get("target", {})
        src_set = src_info.get("node_set_name", default_node_label)
        dst_set = dst_info.get("node_set_name", default_node_label)
        src_idx = src_info.get("indices", [])
        dst_idx = dst_info.get("indices", [])
        if len(src_idx) != len(dst_idx):
            raise ValueError(f"TF-GNN edge set {es_name!r} source ({len(src_idx)}) and target ({len(dst_idx)}) lengths differ.")
        edge_sizes = es_data.get("sizes", [])
        if edge_sizes:
            expected_edges = _sum_component_sizes(edge_sizes, f"edge set {es_name!r}")
            if expected_edges != len(src_idx):
                raise ValueError(
                    f"TF-GNN edge set {es_name!r} sizes sum ({expected_edges}) does not match adjacency length ({len(src_idx)})."
                )
        feats = es_data.get("features", {})
        for ei, (si, di) in enumerate(zip(src_idx, dst_idx)):
            props = {}
            for fname, vals in feats.items():
                if ei < len(vals) and vals[ei] is not None:
                    props[fname] = vals[ei]
            edges_data.append(
                {
                    "source_idx": si,
                    "target_idx": di,
                    "source_set": src_set,
                    "target_set": dst_set,
                    "type": es_name or default_edge_type,
                    "properties": props,
                }
            )

    return nodes_data, edges_data


# ---- Procedures ----------------------------------------------------


@mgp.read_proc
def pyg_export(
    ctx: mgp.ProcCtx,
    node_property_names: mgp.Nullable[mgp.List[str]] = None,
    edge_property_names: mgp.Nullable[mgp.List[str]] = None,
    node_label_property: mgp.Nullable[str] = None,
) -> mgp.Record(json_data=str):
    """Export the graph to a JSON string in PyG format."""
    vertices, edges = _collect_vertices_and_edges(ctx)
    result = _pyg_export_dict(
        vertices,
        edges,
        (list(node_property_names) if node_property_names else None),
        (list(edge_property_names) if edge_property_names else None),
        node_label_property or None,
    )
    return mgp.Record(json_data=json.dumps(result))


@mgp.write_proc
def pyg_import(
    ctx: mgp.ProcCtx,
    json_data: str,
    default_node_label: str = "PyGNode",
    default_edge_type: str = "CONNECTS",
    node_property_names: mgp.Nullable[mgp.List[str]] = None,
    edge_property_names: mgp.Nullable[mgp.List[str]] = None,
    update_existing: bool = False,
) -> mgp.Record(nodes_created=int, edges_created=int, nodes_updated=int):
    """Import PyG data from a JSON string.

    When *update_existing* is ``True`` the procedure does **not** create new
    nodes.  Instead it uses the ``idx_to_node_id`` mapping inside the JSON
    payload to look up existing vertices by their Memgraph internal ID and
    sets the requested *node_property_names* on them.  This is the typical
    "export → inference → write-back" workflow.
    """
    pyg_dict = json.loads(json_data)
    node_props = list(node_property_names) if node_property_names else None
    edge_props = list(edge_property_names) if edge_property_names else None

    if update_existing:
        idx_to_node_id = pyg_dict.get("idx_to_node_id", {})
        num_nodes = pyg_dict.get("num_nodes", 0)
        x = pyg_dict.get("x")
        node_feature_spec = pyg_dict.get("node_feature_spec")
        nodes_updated = 0

        for idx in range(num_nodes):
            orig_id = idx_to_node_id.get(str(idx))
            if orig_id is None:
                continue
            try:
                vertex = ctx.graph.get_vertex_by_id(orig_id)
            except IndexError:
                continue

            props: Dict[str, Any] = {}
            if node_props:
                props = _resolve_named_props(pyg_dict, node_props, idx)
            if x is not None and idx < len(x):
                props = _resolve_feature_fallback(props, x[idx], node_props, node_feature_spec)
            for k, v in props.items():
                vertex.properties.set(k, v)

            nodes_updated += 1

        return mgp.Record(
            nodes_created=0,
            edges_created=0,
            nodes_updated=nodes_updated,
        )

    # -- create mode (default) -------------------------------------------
    nodes_data, edges_data = _pyg_import_data(
        pyg_dict,
        default_node_label,
        default_edge_type,
        node_props,
        edge_props,
    )

    idx_to_vertex: Dict[int, mgp.Vertex] = {}
    nodes_created = 0
    for nd in nodes_data:
        vertex = ctx.graph.create_vertex()
        idx = nd["idx"]
        for label in nd.get("labels", [default_node_label]):
            vertex.add_label(label)
        for k, v in nd.get("properties", {}).items():
            vertex.properties.set(k, v)
        vertex.properties.set("_pyg_idx", idx)
        idx_to_vertex[idx] = vertex
        nodes_created += 1

    edges_created = 0
    for ed in edges_data:
        si, di = ed["source_idx"], ed["target_idx"]
        if si not in idx_to_vertex or di not in idx_to_vertex:
            raise ValueError(f"PyG edge endpoint ({si}, {di}) is outside imported node indices [0, {nodes_created}).")
        e = ctx.graph.create_edge(
            idx_to_vertex[si],
            idx_to_vertex[di],
            mgp.EdgeType(ed.get("type", default_edge_type)),
        )
        for k, v in ed.get("properties", {}).items():
            e.properties.set(k, v)
        edges_created += 1

    return mgp.Record(
        nodes_created=nodes_created,
        edges_created=edges_created,
        nodes_updated=0,
    )


@mgp.read_proc
def tf_export(
    ctx: mgp.ProcCtx,
    node_property_names: mgp.Nullable[mgp.List[str]] = None,
    edge_property_names: mgp.Nullable[mgp.List[str]] = None,
    node_set_name: str = "node",
    edge_set_name: str = "edge",
) -> mgp.Record(json_data=str):
    """Export the graph to a JSON string in TF-GNN format."""
    vertices, edges = _collect_vertices_and_edges(ctx)
    result = _tf_export_dict(
        vertices,
        edges,
        (list(node_property_names) if node_property_names else None),
        (list(edge_property_names) if edge_property_names else None),
        node_set_name,
        edge_set_name,
    )
    return mgp.Record(json_data=json.dumps(result))


@mgp.write_proc
def tf_import(
    ctx: mgp.ProcCtx,
    json_data: str,
    default_node_label: str = "TfGnnNode",
    default_edge_type: str = "CONNECTS",
) -> mgp.Record(nodes_created=int, edges_created=int):
    """Import TF-GNN data from a JSON string."""
    tfgnn_dict = json.loads(json_data)
    nodes_data, edges_data = _tf_import_data(tfgnn_dict, default_node_label, default_edge_type)

    nodes_created = 0
    node_lookup: Dict[tuple, mgp.Vertex] = {}
    for nd in nodes_data:
        vertex = ctx.graph.create_vertex()
        idx = nd["idx"]
        ns = nd.get("node_set", "")
        for label in nd.get("labels", [default_node_label]):
            vertex.add_label(label)
        for k, v in nd.get("properties", {}).items():
            vertex.properties.set(k, v)
        vertex.properties.set("_tfgnn_idx", idx)
        if ns:
            vertex.properties.set("_tfgnn_node_set", ns)
        node_lookup[(ns, idx)] = vertex
        nodes_created += 1

    edges_created = 0
    for ed in edges_data:
        si, di = ed["source_idx"], ed["target_idx"]
        ss = ed.get("source_set", "")
        ds = ed.get("target_set", "")
        sv = node_lookup.get((ss, si))
        dv = node_lookup.get((ds, di))
        if sv is None or dv is None:
            raise ValueError(f"TF-GNN edge endpoint (({ss!r}, {si}), ({ds!r}, {di})) does not match any imported node.")
        e = ctx.graph.create_edge(
            sv,
            dv,
            mgp.EdgeType(ed.get("type", default_edge_type)),
        )
        for k, v in ed.get("properties", {}).items():
            e.properties.set(k, v)
        edges_created += 1

    return mgp.Record(
        nodes_created=nodes_created,
        edges_created=edges_created,
    )
