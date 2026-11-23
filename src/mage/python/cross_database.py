# Copyright 2026 Memgraph Ltd.
#
# Licensed as a Memgraph Enterprise file under the Memgraph Enterprise
# License (the "License"); by using this file, you agree to be bound by the terms of the License, and you may not use
# this file except in compliance with the License. You may obtain a copy of the License at https://memgraph.com/legal.

import base64
import csv
import datetime
import hashlib
import io
import json
import os
import re
import threading
from decimal import Decimal
from typing import Any, Dict, List
from urllib.parse import urljoin, urlparse

import boto3
import duckdb as duckDB
import mgp
import mysql.connector as mysql_connector
import oracledb
import psycopg2
import pyarrow.flight as flight
import pyodbc
from neo4j import GraphDatabase
from neo4j.spatial import Point as Neo4jPoint
from neo4j.time import Date as Neo4jDate
from neo4j.time import DateTime as Neo4jDateTime
from neo4j.time import Duration as Neo4jDuration
from neo4j.time import Time as Neo4jTime

import requests


class Constants:
    BATCH_SIZE = 1000
    COLUMN_NAMES = "column_names"
    CONNECTION = "connection"
    CURSOR = "cursor"
    DATABASE = "database"
    DRIVER = "driver"
    HOST = "host"
    I_COLUMN_NAME = 0
    PASSWORD = "password"
    PORT = "port"
    RESULT = "result"
    SESSION = "session"
    URI_SCHEME = "uri_scheme"
    USERNAME = "username"
    ALREADY_RUNNING_ERROR = (
        "Cross database module with these parameters is already running. "
        "Please wait for it to finish before starting a new one."
    )
    ENTERPRISE_LICENSE_ERROR = (
        "To use the cross_database (migrate) module you need a valid Memgraph Enterprise license."
    )


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------


def _check_enterprise_license() -> None:
    """Cross-database migration is an enterprise-only feature.

    Enterprise builds additionally refuse to load this module at all when no valid license
    is present (kEnterpriseModuleList in src/query/procedure/module.cpp), but that check is
    compiled out of community builds and only runs at load time. This is the per-call guard,
    and therefore the one that covers community builds and licenses that expire or are
    removed while the module is already loaded.
    """
    if not mgp.is_enterprise_valid():
        raise RuntimeError(Constants.ENTERPRISE_LICENSE_ERROR)


_INT64_MIN = -(1 << 63)
_INT64_MAX = (1 << 63) - 1
_state_lock = threading.Lock()


def _push_active_key(stacks: Dict[int, List[str]], cache_key: str) -> None:
    tid = threading.get_ident()
    with _state_lock:
        stacks.setdefault(tid, []).append(cache_key)


def _pop_cleanup_key(stacks: Dict[int, List[str]], state_dict: dict, cleanup_fn) -> None:
    tid = threading.get_ident()
    target_key = None
    with _state_lock:
        thread_stack = stacks.get(tid)
        if thread_stack:
            target_key = thread_stack.pop()
            if not thread_stack:
                stacks.pop(tid, None)
        elif state_dict:
            target_key = next(reversed(state_dict))
    if target_key is not None:
        cleanup_fn(target_key)


def _get_cache_key(
    start_ts: int,
    query: str,
    config: mgp.Map,
    params: mgp.Nullable[mgp.Any] = None,
    config_path: str = "",
) -> str:
    """Cache key from per-query start_timestamp + caller query/config/config_path/params hash (stable across file rotation)."""
    config_dict = dict(config) if config is not None else {}
    config_str = json.dumps(config_dict, sort_keys=True, default=str)

    params_str = ""
    if params is not None:
        if isinstance(params, dict):
            params_str = json.dumps(params, sort_keys=True, default=str)
        elif isinstance(params, (list, tuple)):
            params_str = json.dumps(list(params), sort_keys=False, default=str)
        else:
            params_str = str(params)

    hash_input = f"{start_ts}|{query}|{config_str}|{config_path}|{params_str}"
    return hashlib.sha256(hash_input.encode("utf-8")).hexdigest()


def _query_is_table(table_or_sql: str) -> bool:
    return len(table_or_sql.split()) == 1


def _combine_config(config: mgp.Map, config_path: str) -> Dict[str, Any]:
    assert len(config_path), "Path must not be empty"

    try:
        with open(config_path, "r", encoding="utf-8") as file:
            file_config = json.load(file)
    except Exception as err:
        raise OSError("Could not open/read file.") from err

    merged = dict(config) if config is not None else {}
    merged.update(file_config)
    return merged


def _validate_column_names(column_names) -> List[str]:
    seen = set()
    normalized: List[str] = []
    for column in column_names:
        if column in seen:
            raise ValueError(
                f"Duplicate column name {column!r} in result set; alias repeated columns so every field name is distinct."
            )
        seen.add(column)
        normalized.append(column)
    return normalized


def _convert_decimal(value: Decimal) -> Any:
    if not value.is_finite():
        return str(value)
    exp = value.as_tuple().exponent
    if isinstance(exp, int) and exp >= 0 and value == value.to_integral_value():
        as_int = int(value)
        if _INT64_MIN <= as_int <= _INT64_MAX:
            return as_int
    return format(value, "f")


def _convert_generic_value(value: Any) -> Any:
    if isinstance(value, Decimal):
        return _convert_decimal(value)
    if isinstance(value, list):
        return [_convert_generic_value(item) for item in value]
    if isinstance(value, tuple):
        return [_convert_generic_value(item) for item in value]
    if isinstance(value, dict):
        return {k: _convert_generic_value(v) for k, v in value.items()}
    return value


def _name_row_cells(row_cells, column_names) -> Dict[str, Any]:
    cols = _validate_column_names(column_names)
    cells = list(row_cells)
    if len(cells) != len(cols):
        raise ValueError(f"Row width ({len(cells)}) does not match column header width ({len(cols)}).")
    return {column: _convert_generic_value(value) for column, value in zip(cols, cells)}


def _name_row_cells_mysql(row_cells, column_names) -> Dict[str, Any]:
    """
    Convert MySQL row cells to Memgraph-compatible types.
    Handles MySQL-specific types that might cause PyObject conversion errors.
    """
    cols = _validate_column_names(column_names)
    cells = list(row_cells)
    if len(cells) != len(cols):
        raise ValueError(f"Row width ({len(cells)}) does not match column header width ({len(cols)}).")
    return {column: _convert_mysql_value(value) for column, value in zip(cols, cells)}


def _convert_mysql_value(value: Any) -> Any:
    """
    Convert a MySQL value to a Memgraph-compatible type.
    Returns None for unsupported types and logs a warning.
    """
    if value is None:
        return None

    if isinstance(value, Decimal):
        return _convert_decimal(value)
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        try:
            return value.isoformat()
        except Exception:
            return str(value)
    if isinstance(value, datetime.timedelta):
        return str(value)

    if isinstance(value, (bytes, bytearray)):
        try:
            return value.decode("utf-8")
        except UnicodeDecodeError:
            return base64.b64encode(value).decode("ascii")

    if hasattr(value, "__class__") and "geometry" in str(value.__class__).lower():
        return str(value) if value else None

    if isinstance(value, (int, float, bool)):
        return value

    if isinstance(value, str):
        return value

    if isinstance(value, (list, tuple)):
        return [_convert_mysql_value(item) for item in value]

    if isinstance(value, dict):
        return {k: _convert_mysql_value(v) for k, v in value.items()}

    try:
        str_value = str(value)
        return str_value
    except (ValueError, TypeError):
        return None


def _convert_row_types(row_cells) -> Dict[str, Any]:
    return {column: _convert_generic_value(value) for column, value in row_cells.items()}


def _check_params_type(params: Any, types=(dict, list, tuple)) -> None:
    if not isinstance(params, types):
        raise TypeError(
            "Database query parameter values must be passed in a container of type List[Any] (or Map, if migrating from MySQL or Oracle DB)"
        )


# ---------------------------------------------------------------------------
# Bolt-protocol helpers (Neo4j / Memgraph)
# ---------------------------------------------------------------------------


def _formulate_cypher_query(label_or_rel_or_query: str) -> str:
    stripped = label_or_rel_or_query.strip()
    node_match = re.match(r"^\(\s*:\s*(\w+)\s*\)$", stripped)
    rel_match = re.match(r"^\[\s*:\s*(\w+)\s*\]$", stripped)

    if node_match:
        label = node_match.group(1)
        return f"MATCH (n:{label}) RETURN labels(n) as labels, properties(n) as properties"

    if rel_match:
        rel_type = rel_match.group(1)
        return f"""
    MATCH (n)-[r:{rel_type}]->(m)
    RETURN
        labels(n) as from_labels,
        labels(m) as to_labels,
        properties(n) as from_properties,
        properties(r) as edge_properties,
        properties(m) as to_properties
    """
    return label_or_rel_or_query


def _convert_bolt_value(value):
    """Convert Bolt protocol values (Neo4j/Memgraph) to Python-compatible formats."""
    if value is None:
        return None

    try:
        if isinstance(value, (Neo4jDateTime, Neo4jDate, Neo4jTime)):
            return value.to_native()
        if isinstance(value, Neo4jDuration):
            # Python's datetime.timedelta has no months component. Flatten months into days
            # using Cypher's own convention of 30 days per month (see Cypher Manual,
            # duration functions: "The conversions use general standards (30 days in a
            # month, 24 hours in a day)").
            days = value.days + value.months * 30
            return datetime.timedelta(days=days, seconds=value.seconds, microseconds=value.nanoseconds // 1000)
    except ImportError:
        pass

    if isinstance(value, Neo4jPoint):
        if len(value) == 2:
            return mgp.Point2d(value.x, value.y, value.srid)
        return mgp.Point3d(value.x, value.y, value.z, value.srid)

    if isinstance(value, list):
        return [_convert_bolt_value(item) for item in value]

    if isinstance(value, Decimal):
        return _convert_decimal(value)

    if isinstance(value, dict):
        if value.get("__type") == "mg_enum":
            raise ValueError(f"Enum type is not yet supported in cross-database queries: {value.get('__value')}")
        return {key: _convert_bolt_value(val) for key, val in value.items()}

    return value


def _convert_bolt_record(record):
    """Convert a Bolt protocol record to a Python dict with proper type conversion."""
    if hasattr(record, "keys") and not isinstance(record, dict):
        keys = _validate_column_names(list(record.keys()))
        values = list(record.values())
        if len(values) != len(keys):
            raise ValueError(f"Row width ({len(values)}) does not match column header width ({len(keys)}).")
        return {key: _convert_bolt_value(val) for key, val in zip(keys, values)}
    return {key: _convert_bolt_value(value) for key, value in record.items()}


def _build_uri(config: mgp.Map) -> str:
    host = config.get(Constants.HOST, "localhost")
    port = config.get(Constants.PORT, 7687)
    uri_scheme = config.get(Constants.URI_SCHEME, "bolt")
    return f"{uri_scheme}://{host}:{port}"


def _bolt_init_state(
    state_dict: dict,
    stacks: Dict[int, List[str]],
    start_ts: int,
    label_or_rel_or_query: str,
    config: mgp.Map,
    config_path: str,
    params: mgp.Nullable[mgp.Any],
    default_username: str = "",
    default_password: str = "",
):
    query = _formulate_cypher_query(label_or_rel_or_query)
    cache_key = _get_cache_key(start_ts, query, config, params, config_path=config_path)

    if cache_key in state_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    uri = _build_uri(effective_config)
    username = effective_config.get(Constants.USERNAME, default_username)
    password = effective_config.get(Constants.PASSWORD, default_password)
    database = effective_config.get(Constants.DATABASE, None)

    driver = None
    session = None
    try:
        driver = GraphDatabase.driver(uri, auth=(username, password))
        session = driver.session(database=database) if database else driver.session()
        cypher_params = params if params is not None else {}
        result = session.run(query, parameters=cypher_params)
    except Exception:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        if driver is not None:
            try:
                driver.close()
            except Exception:
                pass
        raise

    state_dict[cache_key] = {
        Constants.DRIVER: driver,
        Constants.SESSION: session,
        Constants.RESULT: result,
    }
    _push_active_key(stacks, cache_key)


def _bolt_fetch_batch(
    state_dict: dict,
    start_ts: int,
    label_or_rel_or_query: str,
    config: mgp.Map,
    config_path: str,
    params: mgp.Nullable[mgp.Any],
):
    query = _formulate_cypher_query(label_or_rel_or_query)
    cache_key = _get_cache_key(start_ts, query, config, params, config_path=config_path)
    result = state_dict[cache_key][Constants.RESULT]

    batch = []
    try:
        for record in result:
            batch.append(mgp.Record(row=_convert_bolt_record(record)))
            if len(batch) >= Constants.BATCH_SIZE:
                break
    except Exception:
        _bolt_cleanup_by_key(state_dict, cache_key)
        raise

    if not batch:
        _bolt_cleanup_by_key(state_dict, cache_key)

    return batch


def _bolt_cleanup_by_key(state_dict: dict, cache_key: str):
    entry = state_dict.pop(cache_key, None)
    if entry is not None:
        session = entry.get(Constants.SESSION)
        driver = entry.get(Constants.DRIVER)
        try:
            if session is not None:
                session.close()
        finally:
            if driver is not None:
                driver.close()


# ---------------------------------------------------------------------------
# Bolt proc (cross_database.bolt)
# ---------------------------------------------------------------------------

_bolt_state = {}
_bolt_stacks: Dict[int, List[str]] = {}


def _init_bolt(
    ctx: mgp.ProcCtx,
    label_or_rel_or_query: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    _check_enterprise_license()
    _bolt_init_state(
        _bolt_state,
        _bolt_stacks,
        ctx.graph.start_timestamp,
        label_or_rel_or_query,
        config,
        config_path,
        params,
    )


def bolt(
    ctx: mgp.ProcCtx,
    label_or_rel_or_query: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    Query any Bolt-compatible database (Memgraph, Neo4j). Can fetch a specific node label, relationship type, or execute a custom Cypher query.

    :param label_or_rel_or_query: Node label, relationship type, or a Cypher query
    :param config: Connection configuration (host, port, username, password, database, uri_scheme)
    :param config_path: Path to a JSON file containing connection parameters
    :param params: Optional query parameters
    :return: Stream of rows from the remote database
    """
    return _bolt_fetch_batch(_bolt_state, ctx.graph.start_timestamp, label_or_rel_or_query, config, config_path, params)


def _cleanup_bolt():
    """Cleanup function called by mgp framework when a Bolt stream ends or aborts early."""
    _pop_cleanup_key(_bolt_stacks, _bolt_state, lambda key: _bolt_cleanup_by_key(_bolt_state, key))


mgp.add_batch_read_proc(bolt, _init_bolt, _cleanup_bolt)


# ---------------------------------------------------------------------------
# Neo4j proc (cross_database.neo4j)
# ---------------------------------------------------------------------------

_neo4j_state = {}
_neo4j_stacks: Dict[int, List[str]] = {}


def _init_neo4j(
    ctx: mgp.ProcCtx,
    label_or_rel_or_query: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    _check_enterprise_license()
    _bolt_init_state(
        _neo4j_state,
        _neo4j_stacks,
        ctx.graph.start_timestamp,
        label_or_rel_or_query,
        config,
        config_path,
        params,
        default_username="neo4j",
        default_password="password",
    )


def neo4j(
    ctx: mgp.ProcCtx,
    label_or_rel_or_query: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    Migrate data from Neo4j to Memgraph. Can migrate a specific node label, relationship type, or execute a custom Cypher query.

    :param label_or_rel_or_query: Node label, relationship type, or a Cypher query
    :param config: Connection configuration for Neo4j
    :param config_path: Path to a JSON file containing connection parameters
    :param params: Optional query parameters
    :return: Stream of rows from Neo4j
    """
    return _bolt_fetch_batch(
        _neo4j_state, ctx.graph.start_timestamp, label_or_rel_or_query, config, config_path, params
    )


def _cleanup_neo4j():
    """Cleanup function called by mgp framework when a Neo4j stream ends or aborts early."""
    _pop_cleanup_key(_neo4j_stacks, _neo4j_state, lambda key: _bolt_cleanup_by_key(_neo4j_state, key))


mgp.add_batch_read_proc(neo4j, _init_neo4j, _cleanup_neo4j)


def _close_sql_entry(state_dict: dict, cache_key: str) -> None:
    entry = state_dict.pop(cache_key, None)
    if entry is None:
        return
    cursor = entry.get(Constants.CURSOR)
    connection = entry.get(Constants.CONNECTION)
    try:
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
    finally:
        if connection is not None:
            try:
                if hasattr(connection, "commit"):
                    connection.commit()
            except Exception:
                pass
            connection.close()


# ---------------------------------------------------------------------------
# MySQL
# ---------------------------------------------------------------------------

mysql_dict = {}
_mysql_stacks: Dict[int, List[str]] = {}


def init_migrate_mysql(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    global mysql_dict

    _check_enterprise_license()

    if params:
        _check_params_type(params)

    sql_query = f"SELECT * FROM {table_or_sql};" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, params, config_path=config_path)

    if cache_key in mysql_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    connection = None
    cursor = None
    try:
        connection = mysql_connector.connect(**effective_config)
        cursor = connection.cursor()
        cursor.execute(sql_query, params)
        column_names = _validate_column_names([column[Constants.I_COLUMN_NAME] for column in cursor.description])
    except Exception:
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass
        raise

    mysql_dict[cache_key] = {
        Constants.CONNECTION: connection,
        Constants.CURSOR: cursor,
        Constants.COLUMN_NAMES: column_names,
    }
    _push_active_key(_mysql_stacks, cache_key)


def mysql(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    With cross_database.mysql you can access MySQL and execute queries.
    The result table is converted into a stream, and returned rows can be
    used to create graph structures. Config must be at least empty map.
    If config_path is passed, every key,value pair from JSON file will
    overwrite any values in config file.

    :param table_or_sql: Table name or an SQL query
    :param config: Connection configuration parameters
                   (as in mysql.connector.connect)
    :param config_path: Path to the JSON file containing configuration
                        parameters (as in mysql.connector.connect)
    :param params: Optionally, queries may be parameterized. In that case,
                   `params` provides parameter values
    :return: The result table as a stream of rows
    """
    global mysql_dict

    sql_query = f"SELECT * FROM {table_or_sql};" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, params, config_path=config_path)
    cursor = mysql_dict[cache_key][Constants.CURSOR]
    column_names = mysql_dict[cache_key][Constants.COLUMN_NAMES]

    try:
        rows = cursor.fetchmany(Constants.BATCH_SIZE)
        result = [mgp.Record(row=_name_row_cells_mysql(row, column_names)) for row in rows]
    except Exception:
        _cleanup_mysql_by_key(cache_key)
        raise

    if not result:
        _cleanup_mysql_by_key(cache_key)

    return result


def _cleanup_mysql_by_key(cache_key: str):
    _close_sql_entry(mysql_dict, cache_key)


def cleanup_migrate_mysql():
    """Cleanup function called by mgp framework when a MySQL stream ends or aborts early."""
    _pop_cleanup_key(_mysql_stacks, mysql_dict, _cleanup_mysql_by_key)


mgp.add_batch_read_proc(mysql, init_migrate_mysql, cleanup_migrate_mysql)


# ---------------------------------------------------------------------------
# SQL Server
# ---------------------------------------------------------------------------

sql_server_dict = {}
_sql_server_stacks: Dict[int, List[str]] = {}


def init_migrate_sql_server(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    global sql_server_dict

    _check_enterprise_license()

    if params:
        _check_params_type(params, (list, tuple))
        exec_params = params
    else:
        exec_params = []

    sql_query = f"SELECT * FROM {table_or_sql};" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, exec_params, config_path=config_path)

    if cache_key in sql_server_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    connection = None
    cursor = None
    try:
        connection = pyodbc.connect(**effective_config)
        cursor = connection.cursor()
        cursor.execute(sql_query, *exec_params)
        column_names = _validate_column_names([column[Constants.I_COLUMN_NAME] for column in cursor.description])
    except Exception:
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass
        raise

    sql_server_dict[cache_key] = {
        Constants.CONNECTION: connection,
        Constants.CURSOR: cursor,
        Constants.COLUMN_NAMES: column_names,
    }
    _push_active_key(_sql_server_stacks, cache_key)


def sql_server(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    With cross_database.sql_server you can access SQL Server and execute queries.
    The result table is converted into a stream, and returned rows can be
    used to create graph structures. Config must be at least empty map.
    If config_path is passed, every key,value pair from JSON file will
    overwrite any values in config file.

    :param table_or_sql: Table name or an SQL query
    :param config: Connection configuration parameters (as in pyodbc.connect)
    :param config_path: Path to the JSON file containing configuration
                        parameters (as in pyodbc.connect)
    :param params: Optionally, queries may be parameterized. In that case,
                   `params` provides parameter values
    :return: The result table as a stream of rows
    """
    global sql_server_dict

    exec_params = params if params else []
    sql_query = f"SELECT * FROM {table_or_sql};" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, exec_params, config_path=config_path)
    cursor = sql_server_dict[cache_key][Constants.CURSOR]
    column_names = sql_server_dict[cache_key][Constants.COLUMN_NAMES]

    try:
        rows = cursor.fetchmany(Constants.BATCH_SIZE)
        result = [mgp.Record(row=_name_row_cells(row, column_names)) for row in rows]
    except Exception:
        _cleanup_sql_server_by_key(cache_key)
        raise

    if not result:
        _cleanup_sql_server_by_key(cache_key)

    return result


def _cleanup_sql_server_by_key(cache_key: str):
    _close_sql_entry(sql_server_dict, cache_key)


def cleanup_migrate_sql_server():
    """Cleanup function called by mgp framework when a SQL Server stream ends or aborts early."""
    _pop_cleanup_key(_sql_server_stacks, sql_server_dict, _cleanup_sql_server_by_key)


mgp.add_batch_read_proc(sql_server, init_migrate_sql_server, cleanup_migrate_sql_server)


# ---------------------------------------------------------------------------
# Oracle DB
# ---------------------------------------------------------------------------

oracle_db_dict = {}
_oracle_db_stacks: Dict[int, List[str]] = {}


def init_migrate_oracle_db(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    global oracle_db_dict

    _check_enterprise_license()

    if params:
        _check_params_type(params)

    sql_query = f"SELECT * FROM {table_or_sql}" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, params, config_path=config_path)

    if cache_key in oracle_db_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    effective_config["disable_oob"] = True

    connection = None
    cursor = None
    try:
        connection = oracledb.connect(**effective_config)
        cursor = connection.cursor()
        if not params:
            cursor.execute(sql_query)
        elif isinstance(params, (list, tuple)):
            cursor.execute(sql_query, params)
        else:
            cursor.execute(sql_query, **params)
        column_names = _validate_column_names([column[Constants.I_COLUMN_NAME] for column in cursor.description])
    except Exception:
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass
        raise

    oracle_db_dict[cache_key] = {
        Constants.CONNECTION: connection,
        Constants.CURSOR: cursor,
        Constants.COLUMN_NAMES: column_names,
    }
    _push_active_key(_oracle_db_stacks, cache_key)


def oracle_db(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    With cross_database.oracle_db you can access Oracle DB and execute queries.
    The result table is converted into a stream, and returned rows can be
    used to create graph structures. Config must be at least empty map.
    If config_path is passed, every key,value pair from JSON file will
    overwrite any values in config file.

    :param table_or_sql: Table name or an SQL query
    :param config: Connection configuration parameters (as in oracledb.connect)
    :param config_path: Path to the JSON file containing configuration
                        parameters (as in oracledb.connect)
    :param params: Optionally, queries may be parameterized. In that case,
                   `params` provides parameter values
    :return: The result table as a stream of rows
    """
    global oracle_db_dict

    sql_query = f"SELECT * FROM {table_or_sql}" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, params, config_path=config_path)
    cursor = oracle_db_dict[cache_key][Constants.CURSOR]
    column_names = oracle_db_dict[cache_key][Constants.COLUMN_NAMES]

    try:
        rows = cursor.fetchmany(Constants.BATCH_SIZE)
        result = [mgp.Record(row=_name_row_cells(row, column_names)) for row in rows]
    except Exception:
        _cleanup_oracle_db_by_key(cache_key)
        raise

    if not result:
        _cleanup_oracle_db_by_key(cache_key)

    return result


def _cleanup_oracle_db_by_key(cache_key: str):
    _close_sql_entry(oracle_db_dict, cache_key)


def cleanup_migrate_oracle_db():
    """Cleanup function called by mgp framework when an Oracle DB stream ends or aborts early."""
    _pop_cleanup_key(_oracle_db_stacks, oracle_db_dict, _cleanup_oracle_db_by_key)


mgp.add_batch_read_proc(oracle_db, init_migrate_oracle_db, cleanup_migrate_oracle_db)


# ---------------------------------------------------------------------------
# PostgreSQL
# ---------------------------------------------------------------------------

postgres_dict = {}
_postgres_stacks: Dict[int, List[str]] = {}


def init_migrate_postgresql(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    global postgres_dict

    _check_enterprise_license()

    if params:
        _check_params_type(params, (list, tuple))
        exec_params = params
    else:
        exec_params = []

    sql_query = f"SELECT * FROM {table_or_sql};" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, exec_params, config_path=config_path)

    if cache_key in postgres_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    connection = None
    cursor = None
    try:
        connection = psycopg2.connect(**effective_config)
        cursor = connection.cursor()
        cursor.execute(sql_query, exec_params)
        column_names = _validate_column_names([column.name for column in cursor.description])
    except Exception:
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass
        raise

    postgres_dict[cache_key] = {
        Constants.CONNECTION: connection,
        Constants.CURSOR: cursor,
        Constants.COLUMN_NAMES: column_names,
    }
    _push_active_key(_postgres_stacks, cache_key)


def postgresql(
    ctx: mgp.ProcCtx,
    table_or_sql: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    With cross_database.postgresql you can access PostgreSQL and execute queries.
    The result table is converted into a stream, and returned rows can be
    used to create graph structures. Config must be at least empty map.
    If config_path is passed, every key,value pair from JSON file will
    overwrite any values in config file.

    :param table_or_sql: Table name or an SQL query
    :param config: Connection configuration parameters (as in psycopg2.connect)
    :param config_path: Path to the JSON file containing configuration
                        parameters (as in psycopg2.connect)
    :param params: Optionally, queries may be parameterized. In that case,
                   `params` provides parameter values
    :return: The result table as a stream of rows
    """
    global postgres_dict

    exec_params = params if params else []
    sql_query = f"SELECT * FROM {table_or_sql};" if _query_is_table(table_or_sql) else table_or_sql
    cache_key = _get_cache_key(ctx.graph.start_timestamp, sql_query, config, exec_params, config_path=config_path)
    cursor = postgres_dict[cache_key][Constants.CURSOR]
    column_names = postgres_dict[cache_key][Constants.COLUMN_NAMES]

    try:
        rows = cursor.fetchmany(Constants.BATCH_SIZE)
        result = [mgp.Record(row=_name_row_cells(row, column_names)) for row in rows]
    except Exception:
        _cleanup_postgresql_by_key(cache_key)
        raise

    if not result:
        _cleanup_postgresql_by_key(cache_key)

    return result


def _cleanup_postgresql_by_key(cache_key: str):
    _close_sql_entry(postgres_dict, cache_key)


def cleanup_migrate_postgresql():
    """Cleanup function called by mgp framework when a PostgreSQL stream ends or aborts early."""
    _pop_cleanup_key(_postgres_stacks, postgres_dict, _cleanup_postgresql_by_key)


mgp.add_batch_read_proc(postgresql, init_migrate_postgresql, cleanup_migrate_postgresql)


# ---------------------------------------------------------------------------
# S3
# ---------------------------------------------------------------------------

s3_dict = {}
_s3_stacks: Dict[int, List[str]] = {}


def init_migrate_s3(
    ctx: mgp.ProcCtx,
    file_path: str,
    config: mgp.Map,
    config_path: str = "",
):
    """
    Initialize an S3 connection and prepare to stream a CSV file.

    :param file_path: S3 file path in the format
                      's3://bucket-name/path/to/file.csv'
    :param config: Configuration map containing AWS credentials
                   (access_key, secret_key, region, etc.). Optionally,
                   'bucket_owner' (12-digit AWS account ID) makes
                   the read fail if the bucket is owned by another account.
    :param config_path: Path to a JSON file containing configuration parameters
    """
    global s3_dict

    _check_enterprise_license()

    if not file_path.startswith("s3://"):
        raise ValueError("Invalid S3 path format. " "Expected 's3://bucket-name/path'.")

    cache_key = _get_cache_key(ctx.graph.start_timestamp, file_path, config, config_path=config_path)

    if cache_key in s3_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    file_path_no_protocol = file_path[5:]
    bucket_name, *key_parts = file_path_no_protocol.split("/")
    s3_key = "/".join(key_parts)

    s3_client = boto3.client(
        "s3",
        aws_access_key_id=effective_config.get("aws_access_key_id", os.getenv("AWS_ACCESS_KEY_ID", None)),
        aws_secret_access_key=effective_config.get("aws_secret_access_key", os.getenv("AWS_SECRET_ACCESS_KEY", None)),
        aws_session_token=effective_config.get("aws_session_token", os.getenv("AWS_SESSION_TOKEN", None)),
        region_name=effective_config.get("region_name", os.getenv("AWS_REGION", None)),
    )

    get_object_kwargs = {"Bucket": bucket_name, "Key": s3_key}
    expected_bucket_owner = effective_config.get("bucket_owner", None)
    if expected_bucket_owner:
        expected_bucket_owner = str(expected_bucket_owner)
        if not re.fullmatch(r"[0-9]{12}", expected_bucket_owner):
            raise ValueError("Invalid 'bucket_owner'. Expected a 12-digit AWS account ID.")
        get_object_kwargs["ExpectedBucketOwner"] = expected_bucket_owner

    body_stream = None
    text_stream = None
    try:
        response = s3_client.get_object(**get_object_kwargs)
        body_stream = response.get("Body")
        text_stream = io.TextIOWrapper(body_stream, encoding="utf-8")
        csv_reader = csv.reader(text_stream)
        column_names = _validate_column_names(next(csv_reader))
    except Exception:
        if text_stream is not None:
            try:
                text_stream.close()
            except Exception:
                pass
        elif body_stream is not None and hasattr(body_stream, "close"):
            try:
                body_stream.close()
            except Exception:
                pass
        raise

    s3_dict[cache_key] = {
        Constants.CONNECTION: text_stream,
        Constants.CURSOR: csv_reader,
        Constants.COLUMN_NAMES: column_names,
    }
    _push_active_key(_s3_stacks, cache_key)


def s3(
    ctx: mgp.ProcCtx,
    file_path: str,
    config: mgp.Map,
    config_path: str = "",
) -> mgp.Record(row=mgp.Map):
    """
    Fetch rows from an S3 CSV file in batches.

    :param file_path: S3 file path in the format
                      's3://bucket-name/path/to/file.csv'
    :param config: AWS S3 connection parameters (AWS credentials, region, etc.),
                   including the optional 'bucket_owner' (12-digit AWS account ID)
    :param config_path: Optional path to a JSON file containing AWS credentials
    :return: The result table as a stream of rows
    """
    global s3_dict

    cache_key = _get_cache_key(ctx.graph.start_timestamp, file_path, config, config_path=config_path)
    csv_reader = s3_dict[cache_key][Constants.CURSOR]
    column_names = s3_dict[cache_key][Constants.COLUMN_NAMES]

    batch_rows = []
    try:
        for _ in range(Constants.BATCH_SIZE):
            try:
                row = next(csv_reader)
            except StopIteration:
                break
            batch_rows.append(mgp.Record(row=_name_row_cells(row, column_names)))
    except Exception:
        _cleanup_s3_by_key(cache_key)
        raise

    if not batch_rows:
        _cleanup_s3_by_key(cache_key)

    return batch_rows


def _cleanup_s3_by_key(cache_key: str):
    entry = s3_dict.pop(cache_key, None)
    if entry is not None:
        stream = entry.get(Constants.CONNECTION)
        if stream is not None and hasattr(stream, "close"):
            try:
                stream.close()
            except Exception:
                pass


def cleanup_migrate_s3():
    """Cleanup function called by mgp framework when an S3 stream ends or aborts early."""
    _pop_cleanup_key(_s3_stacks, s3_dict, _cleanup_s3_by_key)


mgp.add_batch_read_proc(s3, init_migrate_s3, cleanup_migrate_s3)


# ---------------------------------------------------------------------------
# Arrow Flight
# ---------------------------------------------------------------------------

flight_dict = {}
_flight_stacks: Dict[int, List[str]] = {}


def init_migrate_arrow_flight(
    ctx: mgp.ProcCtx,
    query: str,
    config: mgp.Map,
    config_path: str = "",
):
    global flight_dict

    _check_enterprise_license()

    cache_key = _get_cache_key(ctx.graph.start_timestamp, query, config, config_path=config_path)

    if cache_key in flight_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    host = effective_config.get(Constants.HOST, None)
    port = effective_config.get(Constants.PORT, None)
    username = effective_config.get(Constants.USERNAME, "")
    password = effective_config.get(Constants.PASSWORD, "")

    raw_tls = effective_config.get("tls", effective_config.get("encrypted", True))
    if not isinstance(raw_tls, bool):
        raise TypeError("Arrow Flight config 'tls' / 'encrypted' must be a boolean.")
    raw_scheme = effective_config.get(Constants.URI_SCHEME)
    if raw_scheme is not None:
        if raw_scheme not in ("grpc+tls", "grpc"):
            raise ValueError(f"Unsupported Arrow Flight uri_scheme {raw_scheme!r}; expected 'grpc+tls' or 'grpc'.")
        use_tls = raw_scheme == "grpc+tls"
    else:
        use_tls = raw_tls

    has_credentials = bool(username or password)
    if has_credentials and not use_tls:
        raise ValueError(
            "Arrow Flight credentials require TLS (tls=True); refusing to send Basic authorization over plaintext grpc://."
        )

    scheme = "grpc+tls" if use_tls else "grpc"
    connect_kwargs: Dict[str, Any] = {}
    if use_tls:
        tls_root_certs = effective_config.get("tls_root_certs")
        tls_root_certs_path = effective_config.get("tls_root_certs_path")
        if tls_root_certs_path and tls_root_certs is None:
            with open(tls_root_certs_path, "rb") as cert_file:
                tls_root_certs = cert_file.read()
        elif isinstance(tls_root_certs, str):
            tls_root_certs = tls_root_certs.encode("utf-8")
        if tls_root_certs is not None:
            connect_kwargs["tls_root_certs"] = tls_root_certs
        if effective_config.get("cert_chain") is not None:
            connect_kwargs["cert_chain"] = effective_config.get("cert_chain")
        if effective_config.get("private_key") is not None:
            connect_kwargs["private_key"] = effective_config.get("private_key")

    headers = []
    if has_credentials:
        auth_string = f"{username}:{password}".encode("utf-8")
        encoded_auth = base64.b64encode(auth_string).decode("utf-8")
        headers.append((b"authorization", f"Basic {encoded_auth}".encode("utf-8")))

    client = None
    try:
        client = flight.connect(f"{scheme}://{host}:{port}", **connect_kwargs)
        options = flight.FlightCallOptions(headers=headers) if headers else flight.FlightCallOptions()
        flight_info = client.get_flight_info(flight.FlightDescriptor.for_command(query), options)
    except Exception:
        if client is not None and hasattr(client, "close"):
            try:
                client.close()
            except Exception:
                pass
        raise

    flight_dict[cache_key] = {
        Constants.CONNECTION: client,
        Constants.CURSOR: iter(_fetch_flight_data(client, flight_info, options)),
    }
    _push_active_key(_flight_stacks, cache_key)


def _fetch_flight_data(client, flight_info, options):
    """
    Efficiently fetches data in batches from Arrow Flight using RecordBatchReader.
    This prevents high memory usage by avoiding full table loading.
    """
    for endpoint in flight_info.endpoints:
        reader = client.do_get(endpoint.ticket, options)
        if hasattr(reader, "schema") and reader.schema is not None and hasattr(reader.schema, "names"):
            _validate_column_names(list(reader.schema.names))
        for chunk in reader:
            batch = chunk.data
            if hasattr(batch, "schema") and batch.schema is not None and hasattr(batch.schema, "names"):
                _validate_column_names(list(batch.schema.names))
            yield from batch.to_pylist()


def arrow_flight(
    ctx: mgp.ProcCtx,
    query: str,
    config: mgp.Map,
    config_path: str = "",
) -> mgp.Record(row=mgp.Map):
    """
    Execute a SQL query on Arrow Flight and stream results into Memgraph.

    :param query: SQL query to execute
    :param config: Arrow Flight connection configuration (TLS enabled by default)
    :param config_path: Path to a JSON config file
    :return: Stream of rows from Arrow Flight
    """
    global flight_dict

    cache_key = _get_cache_key(ctx.graph.start_timestamp, query, config, config_path=config_path)
    cursor = flight_dict[cache_key][Constants.CURSOR]
    batch = []
    try:
        for _ in range(Constants.BATCH_SIZE):
            try:
                raw_row = next(cursor)
            except StopIteration:
                break
            row = _convert_row_types(raw_row)
            batch.append(mgp.Record(row=row))
    except Exception:
        _cleanup_arrow_flight_by_key(cache_key)
        raise

    if not batch:
        _cleanup_arrow_flight_by_key(cache_key)

    return batch


def _cleanup_arrow_flight_by_key(cache_key: str):
    entry = flight_dict.pop(cache_key, None)
    if entry is not None:
        cursor = entry.get(Constants.CURSOR)
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        client = entry.get(Constants.CONNECTION)
        if client is not None and hasattr(client, "close"):
            try:
                client.close()
            except Exception:
                pass


def cleanup_migrate_arrow_flight():
    """Cleanup function called by mgp framework when an Arrow Flight stream ends or aborts early."""
    _pop_cleanup_key(_flight_stacks, flight_dict, _cleanup_arrow_flight_by_key)


mgp.add_batch_read_proc(arrow_flight, init_migrate_arrow_flight, cleanup_migrate_arrow_flight)


# ---------------------------------------------------------------------------
# DuckDB
# ---------------------------------------------------------------------------

duckdb_dict = {}
_duckdb_stacks: Dict[int, List[str]] = {}


def init_migrate_duckdb(ctx: mgp.ProcCtx, query: str, setup_queries: mgp.Nullable[List[str]] = None):
    """
    Initialize an in-memory DuckDB connection and execute the query.

    :param query: SQL query to execute
    :param setup_queries: Optional list of setup queries to execute before the main query
    """
    global duckdb_dict

    _check_enterprise_license()

    setup_queries_str = json.dumps(setup_queries, sort_keys=False) if setup_queries else ""
    cache_key = _get_cache_key(ctx.graph.start_timestamp, query, {}, setup_queries_str)

    if cache_key in duckdb_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    connection = None
    cursor = None
    try:
        connection = duckDB.connect()
        cursor = connection.cursor()
        if setup_queries is not None:
            for setup_query in setup_queries:
                cursor.execute(setup_query)

        cursor.execute(query)
        column_names = _validate_column_names([desc[0] for desc in cursor.description])
    except Exception:
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass
        raise

    duckdb_dict[cache_key] = {
        Constants.CONNECTION: connection,
        Constants.CURSOR: cursor,
        Constants.COLUMN_NAMES: column_names,
    }
    _push_active_key(_duckdb_stacks, cache_key)


def duckdb(ctx: mgp.ProcCtx, query: str, setup_queries: mgp.Nullable[List[str]] = None) -> mgp.Record(row=mgp.Map):
    """
    Fetch rows from DuckDB in batches.

    :param query: SQL query to execute
    :param setup_queries: Optional list of setup queries to execute before the main query
    :return: The result table as a stream of rows
    """
    global duckdb_dict

    setup_queries_str = json.dumps(setup_queries, sort_keys=False) if setup_queries else ""
    cache_key = _get_cache_key(ctx.graph.start_timestamp, query, {}, setup_queries_str)
    cursor = duckdb_dict[cache_key][Constants.CURSOR]
    column_names = duckdb_dict[cache_key][Constants.COLUMN_NAMES]

    try:
        rows = cursor.fetchmany(Constants.BATCH_SIZE)
        result = [mgp.Record(row=_name_row_cells(row, column_names)) for row in rows]
    except Exception:
        _cleanup_duckdb_by_key(cache_key)
        raise

    if not result:
        _cleanup_duckdb_by_key(cache_key)

    return result


def _cleanup_duckdb_by_key(cache_key: str):
    entry = duckdb_dict.pop(cache_key, None)
    if entry is not None:
        cursor = entry.get(Constants.CURSOR)
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        connection = entry.get(Constants.CONNECTION)
        if connection is not None:
            connection.close()


def cleanup_migrate_duckdb():
    """Cleanup function called by mgp framework when a DuckDB stream ends or aborts early."""
    _pop_cleanup_key(_duckdb_stacks, duckdb_dict, _cleanup_duckdb_by_key)


mgp.add_batch_read_proc(duckdb, init_migrate_duckdb, cleanup_migrate_duckdb)


# ---------------------------------------------------------------------------
# ServiceNow
# ---------------------------------------------------------------------------

servicenow_dict = {}
_servicenow_stacks: Dict[int, List[str]] = {}


def _extract_servicenow_page(response, origin_endpoint: str):
    payload = response.json()
    if not isinstance(payload, dict):
        raise ValueError("ServiceNow response body must be a JSON object.")
    raw_rows = payload.get(Constants.RESULT, [])
    if raw_rows is None:
        raw_rows = []
    if not isinstance(raw_rows, list):
        raise ValueError("ServiceNow response 'result' field must be a list.")

    next_url = None
    resp_links = getattr(response, "links", None)
    if isinstance(resp_links, dict):
        next_entry = resp_links.get("next")
        if isinstance(next_entry, dict) and isinstance(next_entry.get("url"), str):
            next_url = next_entry.get("url")
    if not next_url:
        body_next = payload.get("next")
        if isinstance(body_next, str) and body_next:
            next_url = body_next
        else:
            body_links = payload.get("links")
            if isinstance(body_links, dict):
                cand = body_links.get("next")
                if isinstance(cand, str) and cand:
                    next_url = cand
                elif isinstance(cand, dict):
                    url_val = cand.get("url") or cand.get("href")
                    if isinstance(url_val, str) and url_val:
                        next_url = url_val

    if next_url:
        resolved_next = urljoin(origin_endpoint, next_url)
        origin_parts = urlparse(origin_endpoint)
        next_parts = urlparse(resolved_next)
        if (next_parts.scheme, next_parts.netloc) != (origin_parts.scheme, origin_parts.netloc):
            raise ValueError(
                f"ServiceNow pagination link {resolved_next!r} leaves origin {origin_parts.scheme}://{origin_parts.netloc}."
            )
        next_url = resolved_next

    return raw_rows, next_url


def _iter_servicenow_rows(session, first_rows: list, first_next_url, origin_endpoint: str, headers: dict):
    for row in first_rows:
        yield _convert_generic_value(row)
    seen_urls = {origin_endpoint}
    next_url = first_next_url
    while next_url:
        if next_url in seen_urls:
            raise RuntimeError(f"ServiceNow pagination repeated next-page URL {next_url!r}.")
        seen_urls.add(next_url)
        response = session.get(next_url, headers=headers)
        response.raise_for_status()
        page_rows, next_url = _extract_servicenow_page(response, origin_endpoint)
        for row in page_rows:
            yield _convert_generic_value(row)


def init_migrate_servicenow(
    ctx: mgp.ProcCtx,
    endpoint: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
):
    """
    Initialize the connection to the ServiceNow REST API and fetch the first page of JSON data.

    :param endpoint: ServiceNow API endpoint (full URL)
    :param config: Configuration map containing authentication details (username, password, instance URL, etc.)
    :param config_path: Optional path to a JSON file containing authentication details
    :param params: Optional query parameters for filtering results
    """
    global servicenow_dict

    _check_enterprise_license()

    cache_key = _get_cache_key(ctx.graph.start_timestamp, endpoint, config, params, config_path=config_path)

    if cache_key in servicenow_dict:
        raise RuntimeError(Constants.ALREADY_RUNNING_ERROR)

    effective_config = _combine_config(config=config, config_path=config_path) if len(config_path) > 0 else dict(config)
    auth = (effective_config.get(Constants.USERNAME), effective_config.get(Constants.PASSWORD))
    headers = {"Accept": "application/json"}

    session = requests.Session()
    session.auth = auth
    try:
        response = session.get(endpoint, headers=headers, params=params)
        response.raise_for_status()
        first_rows, first_next_url = _extract_servicenow_page(response, endpoint)
        row_iter = _iter_servicenow_rows(session, first_rows, first_next_url, endpoint, headers)
    except Exception:
        session.close()
        raise

    servicenow_dict[cache_key] = {
        Constants.CONNECTION: session,
        Constants.CURSOR: row_iter,
    }
    _push_active_key(_servicenow_stacks, cache_key)


def servicenow(
    ctx: mgp.ProcCtx,
    endpoint: str,
    config: mgp.Map,
    config_path: str = "",
    params: mgp.Nullable[mgp.Any] = None,
) -> mgp.Record(row=mgp.Map):
    """
    Fetch rows from the ServiceNow REST API in batches across all pages.

    :param endpoint: ServiceNow API endpoint (full URL)
    :param config: Authentication details (username, password, instance URL, etc.)
    :param config_path: Optional path to a JSON file containing authentication details
    :param params: Optional query parameters for filtering results
    :return: The result data as a stream of rows
    """
    global servicenow_dict

    cache_key = _get_cache_key(ctx.graph.start_timestamp, endpoint, config, params, config_path=config_path)
    data_iter = servicenow_dict[cache_key][Constants.CURSOR]

    batch_rows = []
    try:
        for _ in range(Constants.BATCH_SIZE):
            try:
                row = next(data_iter)
            except StopIteration:
                break
            batch_rows.append(mgp.Record(row=row))
    except Exception:
        _cleanup_servicenow_by_key(cache_key)
        raise

    if not batch_rows:
        _cleanup_servicenow_by_key(cache_key)

    return batch_rows


def _cleanup_servicenow_by_key(cache_key: str):
    entry = servicenow_dict.pop(cache_key, None)
    if entry is not None:
        cursor = entry.get(Constants.CURSOR)
        if cursor is not None and hasattr(cursor, "close"):
            try:
                cursor.close()
            except Exception:
                pass
        session = entry.get(Constants.CONNECTION)
        if session is not None and hasattr(session, "close"):
            try:
                session.close()
            except Exception:
                pass


def cleanup_migrate_servicenow():
    """Cleanup function called by mgp framework when a ServiceNow stream ends or aborts early."""
    _pop_cleanup_key(_servicenow_stacks, servicenow_dict, _cleanup_servicenow_by_key)


mgp.add_batch_read_proc(servicenow, init_migrate_servicenow, cleanup_migrate_servicenow)
