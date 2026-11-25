#!/usr/bin/python3
import io
import json
import os
import ssl
import sys

import ldap3
import yaml
from common import add_call_id_to_response, pop_call_id

ALLOWED_ENCRYPTIONS = ("disabled", "ssl", "starttls")
CONFIG_FILE = os.environ.get("MEMGRAPH_LDAP_CONFIG_FILE", "/etc/memgraph/auth/ldap.yaml")

server_config = {}
users_config = {}
roles_config = {}
server = None


def validate_encryption_mode(encryption):
    if not isinstance(encryption, str) or encryption not in ALLOWED_ENCRYPTIONS:
        raise ValueError(
            f"Invalid LDAP encryption setting {encryption!r}; expected one of {', '.join(ALLOWED_ENCRYPTIONS)}."
        )
    return encryption


def init_ldap_server(srv_cfg):
    encryption = validate_encryption_mode(srv_cfg.get("encryption"))
    tls = None
    if encryption != "disabled":
        cert_file = srv_cfg["cert_file"] if srv_cfg.get("cert_file") else None
        key_file = srv_cfg["key_file"] if srv_cfg.get("key_file") else None
        ca_file = srv_cfg["ca_file"] if srv_cfg.get("ca_file") else None
        validate = ssl.CERT_REQUIRED if srv_cfg.get("validate_cert") else ssl.CERT_NONE
        tls = ldap3.Tls(
            local_private_key_file=key_file, local_certificate_file=cert_file, ca_certs_file=ca_file, validate=validate
        )
    use_ssl = encryption == "ssl"
    return ldap3.Server(srv_cfg["host"], port=srv_cfg["port"], tls=tls, use_ssl=use_ssl, get_info=ldap3.ALL)


def load_ldap_config(path):
    global server_config, users_config, roles_config, server
    with open(path, encoding="utf-8") as f:
        config = yaml.safe_load(f)
    server_config = config["server"]
    users_config = config["users"]
    roles_config = config["roles"]
    server = init_ldap_server(server_config)


if os.path.exists(CONFIG_FILE):
    load_ldap_config(CONFIG_FILE)


# Main authentication/authorization function.
def authenticate(username, password):
    if username == "":
        return {"authenticated": False, "role": ""}

    try:
        encryption = validate_encryption_mode(server_config.get("encryption"))
    except ValueError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        return {"authenticated": False, "role": ""}

    if server is None or (encryption == "ssl" and not getattr(server, "ssl", False)):
        print("ERROR: Required LDAP SSL transport is not active!", file=sys.stderr)
        return {"authenticated": False, "role": ""}

    dn = users_config["prefix"] + ldap3.utils.dn.escape_rdn(username) + users_config["suffix"]

    conn = ldap3.Connection(server, dn, password)
    try:
        if encryption == "starttls":
            conn.open()
            if not conn.start_tls() or not getattr(conn, "tls_started", True):
                print("ERROR: Couldn't issue STARTTLS to the LDAP server!", file=sys.stderr)
                return {"authenticated": False, "role": ""}
        if not conn.bind():
            return {"authenticated": False, "role": ""}

        if roles_config["root_dn"] != "":
            search_filter = "(&(objectclass={objclass})({attr}={value}))".format(
                objclass=roles_config["root_objectclass"],
                attr=roles_config["user_attribute"],
                value=ldap3.utils.conv.escape_filter_chars(dn),
            )
            succ = conn.search(
                roles_config["root_dn"],
                search_filter,
                search_scope=ldap3.LEVEL,
                attributes=[roles_config["role_attribute"]],
            )
            if not succ or len(conn.entries) == 0:
                return {"authenticated": True, "role": ""}
            if len(conn.entries) > 1:
                roles = list(map(lambda x: x[roles_config["role_attribute"]].value, conn.entries))
                print(
                    "WARNING: Found more than one role for " "user '" + username + "':",
                    ", ".join(roles) + "!",
                    file=sys.stderr,
                )
                return {"authenticated": True, "role": ""}
            return {"authenticated": True, "role": conn.entries[0][roles_config["role_attribute"]].value}
        return {"authenticated": True, "role": ""}
    finally:
        try:
            conn.unbind()
        except Exception:
            pass


def process_pipe_line(raw_line: bytes) -> bytes:
    call_id = None
    try:
        params = json.loads(raw_line.decode("utf-8"))
        if not isinstance(params, dict):
            raise ValueError("Request payload must be a JSON object")
        call_id = pop_call_id(params)
        ret = authenticate(**params)
    except Exception as exc:
        ret = {"authenticated": False, "role": "", "errors": f"Invalid authentication request: {exc}"}
    add_call_id_to_response(ret, call_id)
    return (json.dumps(ret) + "\n").encode("utf-8")


if __name__ == "__main__":
    if server is None:
        load_ldap_config(CONFIG_FILE)
    input_stream = io.FileIO(1000, mode="r")
    output_stream = io.FileIO(1001, mode="w")
    while True:
        line = input_stream.readline()
        if not line:
            break
        output_stream.write(process_pipe_line(line))
        output_stream.flush()
