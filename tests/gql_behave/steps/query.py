# Copyright 2021 Memgraph Ltd.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
# License, and you may not use this file except in compliance with the Business Source License.
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0, included in the file
# licenses/APL.txt.

# -*- coding: utf-8 -*-

import parser
import re

from behave import given, step, then, when
import database
from neo4j.graph import Node, Path, Relationship
from neo4j.spatial import CartesianPoint, WGS84Point
import yaml

ALL_SIDE_EFFECT_KEYS = (
    "+nodes",
    "-nodes",
    "+relationships",
    "-relationships",
    "+labels",
    "-labels",
    "+properties",
    "-properties",
)


@given("parameters are")
def parameters_step(context):
    context.test_parameters.set_parameters_from_table(context.table)


@then("parameters are")
def parameters_step(context):
    context.test_parameters.set_parameters_from_table(context.table)


@step("having executed")
def having_executed_step(context):
    context.results = database.query(context.text, context, context.test_parameters.get_parameters())


def cleanup_index(index_arg, context, params):
    drop_query = f"DROP INDEX ON {index_arg};"
    database.query(drop_query, context, params)


@step("with new index {index_arg}")
def with_new_index_step(context, index_arg):
    index_creation_query = f"CREATE INDEX ON {index_arg};"
    context.results = database.query(index_creation_query, context, context.test_parameters.get_parameters())
    context.add_cleanup(cleanup_index, index_arg, context, context.test_parameters.get_parameters())


def cleanup_point_index(index_arg, context, params):
    drop_query = f"DROP POINT INDEX ON {index_arg};"
    database.query(drop_query, context, params)


@step("with new point index {index_arg}")
def with_new_index_step(context, index_arg):
    index_creation_query = f"CREATE POINT INDEX ON {index_arg};"
    context.results = database.query(index_creation_query, context, context.test_parameters.get_parameters())
    context.add_cleanup(cleanup_point_index, index_arg, context, context.test_parameters.get_parameters())


@step("with new vector index {index_name} on {index_arg} with dimension {dimension} and capacity {capacity}")
def with_new_index_step(context, index_name, index_arg, dimension, capacity):
    index_creation_query = (
        f"CREATE VECTOR INDEX {index_name} ON {index_arg} WITH CONFIG {{'dimension': {dimension}, 'capacity': {capacity}}};"
    )
    context.results = database.query(index_creation_query, context, context.test_parameters.get_parameters())
    context.add_cleanup(cleanup_vector_index, index_name, context, context.test_parameters.get_parameters())


@step("with new vector edge index {index_name} on {index_arg} with dimension {dimension} and capacity {capacity}")
def with_new_index_step(context, index_name, index_arg, dimension, capacity):
    index_creation_query = (
        f"CREATE VECTOR EDGE INDEX {index_name} ON {index_arg} "
        f"WITH CONFIG {{'dimension': {dimension}, 'capacity': {capacity}}};"
    )
    context.results = database.query(index_creation_query, context, context.test_parameters.get_parameters())
    context.add_cleanup(cleanup_vector_index, index_name, context, context.test_parameters.get_parameters())


def cleanup_vector_index(index_name, context, params):
    drop_query = f"DROP VECTOR INDEX {index_name};"
    database.query(drop_query, context, params)


def cleanup_global_edge_index(property, context, params):
    drop_query = f"DROP GLOBAL EDGE INDEX ON :({property});"
    database.query(drop_query, context, params)


@step("with new edge index :({index_arg})")
def with_new_edge_index_step(context, index_arg):
    index_creation_query = f"CREATE GLOBAL EDGE INDEX ON :({index_arg});"
    context.results = database.query(index_creation_query, context, context.test_parameters.get_parameters())
    context.add_cleanup(cleanup_global_edge_index, index_arg, context, context.test_parameters.get_parameters())


def cleanup_edge_type_index(index_arg, context, params):
    database.query(f"DROP EDGE INDEX ON {index_arg};", context, params)


@step("with new edge type index {index_arg}")
def with_new_edge_type_index_step(context, index_arg):
    context.results = database.query(
        f"CREATE EDGE INDEX ON {index_arg};", context, context.test_parameters.get_parameters()
    )
    context.add_cleanup(cleanup_edge_type_index, index_arg, context, context.test_parameters.get_parameters())


def _cypher_type_to_mgp_expr(cypher_type_raw):
    text = cypher_type_raw.strip()
    nullable = text.endswith("?")
    if nullable:
        text = text[:-1].strip()
    upper = text.upper()
    if upper.startswith("LIST"):
        inner_match = re.match(r"(?i)^LIST\s+OF\s+(.+)$", text)
        inner_expr = _cypher_type_to_mgp_expr(inner_match.group(1)) if inner_match else "mgp.Nullable[mgp.Any]"
        base_expr = f"mgp.List[{inner_expr}]"
    else:
        mapping = {
            "INTEGER": "int",
            "INT": "int",
            "FLOAT": "float",
            "NUMBER": "mgp.Number",
            "STRING": "str",
            "BOOLEAN": "bool",
            "BOOL": "bool",
            "MAP": "mgp.Map",
            "NODE": "mgp.Vertex",
            "VERTEX": "mgp.Vertex",
            "RELATIONSHIP": "mgp.Edge",
            "EDGE": "mgp.Edge",
            "PATH": "mgp.Path",
            "ANY": "mgp.Any",
        }
        base_expr = mapping.get(upper, "mgp.Any")
    if nullable and base_expr != "mgp.Any":
        return f"mgp.Nullable[{base_expr}]"
    if nullable and base_expr == "mgp.Any":
        return "mgp.Nullable[mgp.Any]"
    return base_expr


def _split_top_level_fields(fields_str):
    parts = []
    cur = []
    depth = 0
    for ch in fields_str:
        if ch in ("(", "[", "{"):
            depth += 1
            cur.append(ch)
        elif ch in (")", "]", "}"):
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            item = "".join(cur).strip()
            if item:
                parts.append(item)
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        parts.append(tail)
    return parts


def parse_procedure_signature(signature):
    cleaned = signature.strip().rstrip(":").strip()
    match = re.match(r"^([A-Za-z_][A-Za-z0-9_.]*)\s*\((.*)\)\s*::\s*(.+)$", cleaned)
    if not match:
        raise ValueError(f"Unsupported procedure signature: {signature!r}")
    qualified_name, args_raw, returns_raw = match.group(1), match.group(2).strip(), match.group(3).strip()
    if "." not in qualified_name:
        raise ValueError(f"Procedure signature must include a module prefix: {qualified_name!r}")
    module_name, proc_name = qualified_name.rsplit(".", 1)

    inputs = []
    if args_raw:
        for field in _split_top_level_fields(args_raw):
            name, cypher_type = field.split("::", 1)
            inputs.append((name.strip(), cypher_type.strip()))

    outputs = []
    if returns_raw.upper() != "VOID":
        if not (returns_raw.startswith("(") and returns_raw.endswith(")")):
            raise ValueError(f"Unsupported procedure return specification: {returns_raw!r}")
        inner_returns = returns_raw[1:-1].strip()
        if inner_returns:
            for field in _split_top_level_fields(inner_returns):
                name, cypher_type = field.split("::", 1)
                outputs.append((name.strip(), cypher_type.strip()))

    return module_name, proc_name, inputs, outputs


def parse_cypher_table_cell(raw):
    text = raw.strip()
    if text == "" or text == "null":
        return None
    if text == "true":
        return True
    if text == "false":
        return False
    if len(text) >= 2 and text[0] == "'" and text[-1] == "'":
        return text[1:-1]
    return yaml.safe_load(text)


def extract_procedure_rows(table, inputs, outputs):
    if table is None or not getattr(table, "headings", None):
        return []
    headings = [h.strip() for h in table.headings]
    if len(headings) == 1 and headings[0] == "":
        return []
    expected_cols = [name for name, _ in inputs] + [name for name, _ in outputs]
    assert headings == expected_cols, f"Procedure table columns {headings} do not match signature columns {expected_cols}"
    rows = []
    for row in table:
        rows.append({col: parse_cypher_table_cell(row[col]) for col in headings})
    return rows


def build_procedure_module_header():
    return (
        "from functools import wraps\n"
        "import mgp\n\n"
        "def _arg_matches(actual, expected):\n"
        "    if actual is None or expected is None:\n"
        "        return actual is expected\n"
        "    if isinstance(actual, bool) != isinstance(expected, bool):\n"
        "        return False\n"
        "    return actual == expected\n\n"
        "def _register_tck_proc(proc_name, fn, inputs, outputs):\n"
        "    @wraps(fn)\n"
        "    def wrapper(graph, args):\n"
        "        return fn(*args)\n"
        "    wrapper.__name__ = proc_name\n"
        "    wrapper.__qualname__ = proc_name\n"
        "    mgp_proc = mgp._mgp.Module.add_read_procedure(mgp._mgp._MODULE, wrapper)\n"
        "    for arg_name, arg_type in inputs:\n"
        "        mgp_proc.add_arg(arg_name, mgp._typing_to_cypher_type(arg_type))\n"
        "    for out_name, out_type in outputs:\n"
        "        mgp_proc.add_result(out_name, mgp._typing_to_cypher_type(out_type))\n"
        "    return wrapper\n"
    )


def build_procedure_snippet(proc_name, inputs, outputs, rows, index_suffix):
    in_names = [name for name, _ in inputs]
    out_names = [name for name, _ in outputs]
    inputs_code = ", ".join(f"({name!r}, {_cypher_type_to_mgp_expr(ctype)})" for name, ctype in inputs)
    outputs_code = ", ".join(f"({name!r}, {_cypher_type_to_mgp_expr(ctype)})" for name, ctype in outputs)

    fn_var = f"_proc_impl_{index_suffix}"
    rows_var = f"_ROWS_{index_suffix}"
    in_var = f"_IN_{index_suffix}"
    out_var = f"_OUT_{index_suffix}"
    body_lines = [
        f"\n{rows_var} = {rows!r}",
        f"{in_var} = {in_names!r}",
        f"{out_var} = {out_names!r}",
        f"def {fn_var}(*args):",
    ]
    if not outputs:
        body_lines.append("    return None")
    else:
        body_lines.extend(
            [
                "    matched = []",
                f"    for row in {rows_var}:",
                f"        if all(_arg_matches(arg_val, row[col]) for arg_val, col in zip(args, {in_var})):",
                f"            matched.append(mgp.Record(**{{col: row[col] for col in {out_var}}}))",
                "    return matched",
            ]
        )
    body_lines.append(f"_register_tck_proc({proc_name!r}, {fn_var}, [{inputs_code}], [{outputs_code}])")
    return "\n".join(body_lines) + "\n"


def cleanup_installed_module(module_name, context):
    paths = getattr(context, "installed_module_paths", {})
    sources = getattr(context, "installed_module_sources", {})
    module_path = paths.pop(module_name, None)
    sources.pop(module_name, None)
    if not module_path:
        return
    session = context.driver.session()
    try:
        list(session.run("CALL mg.delete_module_file($path);", {"path": module_path}))
    finally:
        session.close()


def install_procedure_from_step(context, signature):
    module_name, proc_name, inputs, outputs = parse_procedure_signature(signature)
    rows = extract_procedure_rows(getattr(context, "table", None), inputs, outputs)
    if not hasattr(context, "installed_module_sources"):
        context.installed_module_sources = {}
    if not hasattr(context, "installed_module_paths"):
        context.installed_module_paths = {}

    existing_source = context.installed_module_sources.get(module_name)
    proc_index = 0 if existing_source is None else existing_source.count("_register_tck_proc(")
    snippet = build_procedure_snippet(proc_name, inputs, outputs, rows, proc_index)
    full_source = (build_procedure_module_header() if existing_source is None else existing_source) + snippet

    session = context.driver.session()
    try:
        if module_name in context.installed_module_paths:
            module_path = context.installed_module_paths[module_name]
            list(
                session.run(
                    "CALL mg.update_module_file($path, $content);",
                    {"path": module_path, "content": full_source},
                )
            )
        else:
            filename = f"{module_name}.py"
            records = list(
                session.run(
                    "CALL mg.create_module_file($filename, $content) YIELD path RETURN path;",
                    {"filename": filename, "content": full_source},
                )
            )
            assert records, f"mg.create_module_file returned no path for {filename}"
            row_vals = list(records[0].values())
            context.installed_module_paths[module_name] = row_vals[0]
            context.add_cleanup(cleanup_installed_module, module_name, context)
        context.installed_module_sources[module_name] = full_source
    finally:
        session.close()


@step("there exists a procedure {signature}:")
def there_exists_a_procedure_colon_step(context, signature):
    install_procedure_from_step(context, signature)


@step("there exists a procedure {signature}")
def there_exists_a_procedure_step(context, signature):
    install_procedure_from_step(context, signature)


def canonical_prop_value(value):
    if isinstance(value, bool):
        return ("bool", value)
    if isinstance(value, (int, float)):
        return ("num", value)
    if isinstance(value, list):
        return ("list", tuple(canonical_prop_value(item) for item in value))
    if isinstance(value, dict):
        return ("map", tuple(sorted((k, canonical_prop_value(v)) for k, v in value.items())))
    return ("val", to_string(value))


def snapshot_graph(context):
    node_ids = set()
    rel_ids = set()
    graph_labels = set()
    prop_entries = set()

    session = context.driver.session()
    try:
        node_records = list(
            session.run("MATCH (n) RETURN id(n) AS id, labels(n) AS labels, properties(n) AS props;")
        )
        for rec in node_records:
            rec_map = dict(zip(rec.keys(), rec.values()))
            nid = rec_map["id"]
            node_ids.add(nid)
            for lbl in rec_map["labels"] or ():
                graph_labels.add(lbl)
            props = rec_map["props"] or {}
            for k, v in props.items():
                if v is not None:
                    prop_entries.add(("node", nid, k, canonical_prop_value(v)))

        rel_records = list(
            session.run("MATCH ()-[r]->() RETURN id(r) AS id, type(r) AS type, properties(r) AS props;")
        )
        for rec in rel_records:
            rec_map = dict(zip(rec.keys(), rec.values()))
            rid = rec_map["id"]
            rel_ids.add(rid)
            props = rec_map["props"] or {}
            for k, v in props.items():
                if v is not None:
                    prop_entries.add(("rel", rid, k, canonical_prop_value(v)))
    finally:
        session.close()

    return (node_ids, rel_ids, graph_labels, prop_entries)


def compute_side_effect_deltas(before_snapshot, after_snapshot):
    before_nodes, before_rels, before_labels, before_props = before_snapshot
    after_nodes, after_rels, after_labels, after_props = after_snapshot
    return {
        "+nodes": len(after_nodes - before_nodes),
        "-nodes": len(before_nodes - after_nodes),
        "+relationships": len(after_rels - before_rels),
        "-relationships": len(before_rels - after_rels),
        "+labels": len(after_labels - before_labels),
        "-labels": len(before_labels - after_labels),
        "+properties": len(after_props - before_props),
        "-properties": len(before_props - after_props),
    }


def print_actual_query(context):
    """Print the actual query if it differs from the original (e.g., with USING PARALLEL EXECUTION)."""
    actual_query = getattr(context, "last_executed_query", None)
    if actual_query and actual_query != context.text:
        print(f'      Actual query executed:\n      """\n      {actual_query}\n      """')


@when("executing query")
def executing_query_step(context):
    before_snapshot = snapshot_graph(context)
    context.results = database.query(context.text, context, context.test_parameters.get_parameters())
    if getattr(context, "exception", None) is None:
        after_snapshot = snapshot_graph(context)
        context.side_effect_deltas = compute_side_effect_deltas(before_snapshot, after_snapshot)
    else:
        context.side_effect_deltas = None
    print_actual_query(context)


@when("executing control query")
def executing_query_step(context):
    context.results = database.query(context.text, context, context.test_parameters.get_parameters())
    print_actual_query(context)


def parse_props(props_key_value):
    """
    Function used to parse properties from map of properties to string.

    @param prop_json:
        Dictionary, map of properties of graph element.
    @return:
        Map of properties in string format.
    """
    if not props_key_value:
        return ""
    properties = "{"
    for key, value in props_key_value:
        if value is None:
            properties += key + ": null, "
        elif isinstance(value, str):
            properties += key + ": " + "'" + value + "', "
        elif isinstance(value, bool):
            if value:
                properties += key + ": true, "
            else:
                properties += key + ": false, "
        elif isinstance(value, CartesianPoint):
            properties += key + ": POINT({" + f"x:{value.x}, y:{value.y},"
            properties += (
                f"{('z:' + str(value.z) + ', ') if hasattr(value, 'z') else ''}" + f" srid:{value.srid}" + "})" + ", "
            )
        elif isinstance(value, WGS84Point):
            properties += key + ": POINT({" + f"longitude:{value.longitude}, latitude:{value.latitude},"
            properties += (
                f"{'height:' + str(value.height) + ', ' if hasattr(value, 'height') else ''}"
                + f" srid:{value.srid}"
                + "})"
                + ", "
            )
        else:
            properties += key + ": " + str(value) + ", "
    properties = properties[:-2]
    properties += "}"
    return properties


def to_string(element):
    """
    Function used to parse result from database to string. Format of
    the string is same as format of a result given in cucumber test.

    @param element:
        Can be None, string, bool, number, list, dict or any of neo4j.graph
        Node, Path, Relationship. Element which will be parsed.
    @return:
        String of parsed element.
    """
    if element is None:
        return "null"

    if isinstance(element, Node):
        sol = "("
        if element.labels:
            sol += ":" + ": ".join(element.labels)

        if element.keys():
            if element.labels:
                sol += " "
            sol += parse_props(element.items())

        sol += ")"
        return sol

    elif isinstance(element, Relationship):
        sol = "[:"
        if element.type:
            sol += element.type
        if element.keys():
            sol += " "
        sol += parse_props(element.items())
        sol += "]"
        return sol

    elif isinstance(element, Path):
        edges = []
        nodes = []

        for rel in element.relationships:
            edges.append([rel.start_node.id, to_string(rel)])

        for node in element.nodes:
            nodes.append([node.id, to_string(node)])

        sol = "<"
        for i in range(0, len(edges)):
            if edges[i][0] == nodes[i][0]:
                sol += nodes[i][1] + "-" + edges[i][1] + "->"
            else:
                sol += nodes[i][1] + "<-" + edges[i][1] + "-"

        sol += nodes[len(edges)][1]
        sol += ">"

        return sol

    elif isinstance(element, str):
        return "'" + element + "'"

    elif isinstance(element, list):
        sol = "["
        el_str = []
        for el in element:
            el_str.append(to_string(el))
        sol += ", ".join(el_str)
        sol += "]"

        return sol

    elif isinstance(element, bool):
        if element:
            return "true"
        return "false"

    elif isinstance(element, dict):
        if len(element) == 0:
            return "{}"
        sol = "{"
        for key, val in element.items():
            sol += key + ":" + to_string(val) + ","
        sol = sol[:-1] + "}"
        return sol

    elif isinstance(element, float):
        if "e" in str(element):
            if str(element)[-3] == "-":
                zeroes = int(str(element)[-2:]) - 1
                num_str = ""
                if str(element)[0] == "-":
                    num_str += "-"
                num_str += "." + zeroes * "0" + str(element)[:-4].replace("-", "").replace(".", "")
                return num_str

    return str(element)


def strip_unquoted_whitespace(text):
    """Strip whitespace outside quoted string literals while preserving spaces inside quotes."""
    out = []
    quote_char = None
    escaped = False
    for ch in text:
        if quote_char is not None:
            out.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote_char:
                quote_char = None
        else:
            if ch in ("'", '"'):
                quote_char = ch
                out.append(ch)
            elif not ch.isspace():
                out.append(ch)
    return "".join(out)


def get_result_rows(context, ignore_order):
    """
    Return query results as a list of row tuples aligned to context.table.headings,
    preserving column-to-row association and spaces inside quoted strings.
    """
    headings = list(context.table.headings)
    result_rows = []
    for result in context.results:
        keys = list(result.keys())
        values = list(result.values())
        assert len(keys) == len(headings) and set(keys) == set(headings), (
            f"Result columns {keys} do not match expected columns {headings}"
        )
        row_map = dict(zip(keys, values))
        row_tuple = tuple(
            col + ":" + parser.parse(strip_unquoted_whitespace(to_string(row_map[col]).replace("\n", "\\n")), ignore_order)
            for col in headings
        )
        result_rows.append(row_tuple)
    return result_rows


def get_expected_rows(context, ignore_order):
    """
    Return expected table rows as a list of row tuples aligned to context.table.headings,
    preserving column-to-row association and spaces inside quoted strings.
    """
    headings = list(context.table.headings)
    expected_rows = []
    for row in context.table:
        row_tuple = tuple(
            col + ":" + parser.parse(strip_unquoted_whitespace(row[col]), ignore_order)
            for col in headings
        )
        expected_rows.append(row_tuple)
    return expected_rows


def validate(context, ignore_order):
    """
    Check if results from database match expected rows in any row order while keeping
    all columns of each row together.
    """
    result_rows = get_result_rows(context, ignore_order)
    expected_rows = get_expected_rows(context, ignore_order)

    context.log.info("Expected: %s", str(expected_rows))
    context.log.info("Results:  %s", str(result_rows))
    assert len(expected_rows) == len(result_rows)

    for expected_row in expected_rows:
        if expected_row in result_rows:
            result_rows.remove(expected_row)
        else:
            assert False, f"Expected row {expected_row} not found in remaining results {result_rows}"


def validate_in_order(context, ignore_order):
    """
    Check if results from database match expected rows in exact row order.
    """
    result_rows = get_result_rows(context, ignore_order)
    expected_rows = get_expected_rows(context, ignore_order)

    context.log.info("Expected: %s", str(expected_rows))
    context.log.info("Results:  %s", str(result_rows))
    assert len(expected_rows) == len(result_rows)

    for idx, expected_row in enumerate(expected_rows):
        if expected_row != result_rows[idx]:
            assert False, f"Row {idx} mismatch: expected {expected_row}, got {result_rows[idx]}"


def has_aggregation_functions(query):
    """
    Check if query contains aggregation functions that produce non-deterministic order.
    Returns True if query contains collect, map, or project aggregations.
    """
    query_upper = query.upper()
    patterns = [
        r"\bCOLLECT\s*\(",
        r"\bMAP\s*\(",
        r"\bPROJECT\s*\(",
    ]
    for pattern in patterns:
        if re.search(pattern, query_upper):
            return True
    return False


def should_ignore_row_order(context):
    """
    Check if row order should be ignored for validation.
    Returns True if parallel execution is enabled and the query doesn't have ORDER BY.
    """
    parallel_execution = getattr(context.config, "parallel_execution", False)
    if not parallel_execution:
        return False

    last_query = getattr(context, "last_executed_query", "")
    return "ORDER BY" not in last_query.upper()


def should_ignore_aggregation_order(context):
    """
    Check if aggregation order should be ignored for validation.
    Returns True if parallel execution is enabled and the query contains aggregation functions
    (collect, map, project) that produce non-deterministic order.
    """
    parallel_execution = getattr(context.config, "parallel_execution", False)
    if not parallel_execution:
        return False

    last_query = getattr(context, "last_executed_query", "")
    return has_aggregation_functions(last_query)


@then("the result should be")
def expected_result_step(context):
    ignore_agg_order = should_ignore_aggregation_order(context)
    if ignore_agg_order:
        context.log.info("Parallel execution with aggregations: ignoring order within aggregated collections")
    validate(context, ignore_agg_order)
    check_exception(context)


@then("the result should be, in order")
def expected_result_step(context):
    ignore_row_order = should_ignore_row_order(context)
    ignore_agg_order = should_ignore_aggregation_order(context)

    if ignore_row_order:
        context.log.info("Parallel execution without ORDER BY: ignoring row order")
        if ignore_agg_order:
            context.log.info("Parallel execution with aggregations: ignoring order within aggregated collections")
        validate(context, ignore_agg_order)
    else:
        if ignore_agg_order:
            context.log.info("Parallel execution with aggregations: ignoring order within aggregated collections")
        validate_in_order(context, ignore_agg_order)
    check_exception(context)


@then("the result should be (ignoring element order for lists)")
def expected_result_step(context):
    validate(context, True)
    check_exception(context)


def check_exception(context):
    if context.exception is not None:
        context.log.info("Exception when executing query!")
        assert False, f"Unexpected exception when executing query: {context.exception}"


@then("the result should be empty")
def empty_result_step(context):
    assert len(context.results) == 0
    check_exception(context)


@then("the side effects should be")
def side_effects_step(context):
    check_exception(context)
    actual = getattr(context, "side_effect_deltas", None)
    assert actual is not None, "No graph side-effect snapshot recorded before/after executing query."
    expected = {key: 0 for key in ALL_SIDE_EFFECT_KEYS}
    assert context.table is not None, "Expected a side-effects table."
    raw_pairs = []
    if len(context.table.headings) >= 2:
        raw_pairs.append((context.table.headings[0].strip(), context.table.headings[1].strip()))
    for row in context.table:
        raw_pairs.append((row[0].strip(), row[1].strip()))
    for key, raw_count in raw_pairs:
        assert key in expected, f"Unknown side-effect key: {key!r}"
        expected[key] = int(raw_count)
    assert actual == expected, f"Graph side effects mismatch: expected {expected}, got {actual}"


@then("no side effects")
def side_effects_step(context):
    check_exception(context)
    actual = getattr(context, "side_effect_deltas", None)
    assert actual is not None, "No graph side-effect snapshot recorded before/after executing query."
    expected = {key: 0 for key in ALL_SIDE_EFFECT_KEYS}
    assert actual == expected, f"Expected no graph side effects, got {actual}"
