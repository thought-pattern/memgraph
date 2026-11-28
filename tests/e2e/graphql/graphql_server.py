import atexit
import collections.abc
import json
import os.path
import socket
import subprocess
import time
from uuid import UUID

import requests

try:
    from memgraph import PORT_REMAP

    def _effective_port(port):
        # Under runner_parallel.py ports move into the worker's window; node has to be told explicitly.
        return PORT_REMAP.map_port(port)

except Exception:  # Plain runner.py: nothing is remapped.

    def _effective_port(port):
        return port


BOLT_PORT = 7687
SERVER_PORT = 4000


class GraphQLServer:
    def __init__(self, config_file_path: str):
        # The Python side is remapped transparently, so the well-known port is used here.
        self.url = f"http://127.0.0.1:{SERVER_PORT}"

        self.graphql_lib = subprocess.Popen(
            ["node", os.path.join("graphql/graphql_library_config/server.js"), config_file_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={
                **os.environ,
                "GRAPHQL_E2E_BOLT_PORT": str(_effective_port(BOLT_PORT)),
                "GRAPHQL_E2E_SERVER_PORT": str(_effective_port(SERVER_PORT)),
            },
        )

        self.__wait_process_to_init(BOLT_PORT)
        self.__wait_process_to_init(SERVER_PORT)
        atexit.register(self.__shut_down)
        print(f"GraphQLServer started")

    def send_query(self, query: str, variables=None, timeout=5.0) -> requests.Response:
        headers = {
            "Content-Type": "application/json",
        }
        json = {"query": query, "variables": variables or {}}

        try:
            response = requests.post(self.url, headers=headers, json=json, timeout=timeout)
        except requests.exceptions.Timeout as err:
            print("Request to GraphQL server has timed out. Details:", err)
        else:
            return response

    def __wait_process_to_init(self, port, timeout_s=60):
        host = "127.0.0.1"
        deadline = time.monotonic() + timeout_s
        try:
            while True:
                # Create a socket object
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                    s.settimeout(5)
                    result = s.connect_ex((host, port))
                    if result == 0:
                        break
                # Fail instead of hanging when the node server dies (e.g. graphql/setup.sh was never run).
                if self.graphql_lib.poll() is not None:
                    stderr = self.graphql_lib.stderr.read().decode(errors="replace")
                    raise RuntimeError(f"GraphQL server exited with code {self.graphql_lib.returncode}:\n{stderr}")
                if time.monotonic() > deadline:
                    raise RuntimeError(f"Port {port} did not open within {timeout_s}s")
                time.sleep(0.1)

        except socket.error as e:
            print(f"Error occurred while checking port {port}: {e}")
            return False

    def __shut_down(self):
        self.graphql_lib.kill()
        ls = subprocess.Popen(("lsof", "-t", f"-i:{_effective_port(SERVER_PORT)}"), stdout=subprocess.PIPE)
        subprocess.check_output(("xargs", "-r", "kill"), stdin=ls.stdout)
        ls.wait()


def _ordered(obj: any) -> any:
    if isinstance(obj, dict):
        return sorted((k, _ordered(v)) for k, v in obj.items())
    if isinstance(obj, list):
        return sorted(_ordered(x) for x in obj)
    else:
        return obj


def _flatten(x: any) -> list:
    result = []
    for el in x:
        if isinstance(x, collections.abc.Iterable) and not isinstance(el, None | str | int | float):
            result.extend(_flatten(el))
        else:
            result.append(el)
    return result


def _valid_uuid(uuid_to_test: any, version: int = 4) -> any:
    try:
        uuid_obj = UUID(uuid_to_test, version=version)
    except (ValueError, AttributeError):
        return False
    return str(uuid_obj) == uuid_to_test


def _values_match(
    expected_val: any,
    actual_val: any,
    *,
    preserve_order: bool = True,
    allow_generated_uuid: bool = False,
) -> bool:
    if expected_val is None:
        return actual_val is None
    if isinstance(expected_val, bool):
        return isinstance(actual_val, bool) and expected_val == actual_val
    if isinstance(expected_val, (int, float)):
        return isinstance(actual_val, (int, float)) and not isinstance(actual_val, bool) and expected_val == actual_val
    if isinstance(expected_val, str):
        if not isinstance(actual_val, str):
            return False
        if expected_val == actual_val:
            return True
        return allow_generated_uuid and _valid_uuid(expected_val) and _valid_uuid(actual_val)
    if isinstance(expected_val, dict):
        if not isinstance(actual_val, dict) or set(expected_val.keys()) != set(actual_val.keys()):
            return False
        return all(
            _values_match(
                expected_val[key],
                actual_val[key],
                preserve_order=preserve_order,
                allow_generated_uuid=allow_generated_uuid,
            )
            for key in expected_val
        )
    if isinstance(expected_val, list):
        if not isinstance(actual_val, list) or len(expected_val) != len(actual_val):
            return False
        if preserve_order:
            return all(
                _values_match(
                    exp_item,
                    act_item,
                    preserve_order=preserve_order,
                    allow_generated_uuid=allow_generated_uuid,
                )
                for exp_item, act_item in zip(expected_val, actual_val)
            )
        remaining = list(actual_val)
        for exp_item in expected_val:
            matched_idx = next(
                (
                    idx
                    for idx, candidate in enumerate(remaining)
                    if _values_match(
                        exp_item,
                        candidate,
                        preserve_order=preserve_order,
                        allow_generated_uuid=allow_generated_uuid,
                    )
                ),
                None,
            )
            if matched_idx is None:
                return False
            remaining.pop(matched_idx)
        return True
    return expected_val == actual_val


def server_returned_expected(
    expected_string: str,
    server_response: requests.Response,
    *,
    preserve_order: bool = True,
    allow_generated_uuid: bool = False,
) -> bool:
    if server_response is None or getattr(server_response, "status_code", 200) >= 400:
        return False
    expected_json = json.loads(expected_string)
    server_response_json = json.loads(server_response.text)
    if "errors" in server_response_json and "errors" not in expected_json:
        return False
    return _values_match(
        expected_json,
        server_response_json,
        preserve_order=preserve_order,
        allow_generated_uuid=allow_generated_uuid,
    )


def get_uuid_from_response(response: requests.Response) -> list:
    response_json = json.loads(response.text)
    flattened_response = _flatten(_ordered(response_json))
    uuids = []
    for item in flattened_response:
        if _valid_uuid(item):
            uuids.append(str(item))
    return uuids
