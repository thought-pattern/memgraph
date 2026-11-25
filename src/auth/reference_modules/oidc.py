#!/usr/bin/python3

import base64
import hashlib
import hmac
import io
import json
import os
import time

import jwt
import requests


def _expected_issuer(scheme: str, config: dict, token_type: str) -> str:
    if scheme == "oidc-entra-id":
        explicit = config.get("issuer", "")
        if explicit:
            return explicit
        tenant_id = config.get("tenant_id", "")
        return f"https://login.microsoftonline.com/{tenant_id}/v2.0" if tenant_id else ""
    if scheme == "oidc-okta":
        return config.get("id_issuer", "")
    if scheme == "oidc-custom":
        if token_type == "access":
            return config.get("access_token_issuer") or config.get("issuer", "")
        return config.get("id_token_issuer") or config.get("issuer", "")
    return ""


def _compute_at_hash(raw_access_token: str) -> str:
    digest = hashlib.sha256(raw_access_token.encode("ascii")).digest()
    left_half = digest[: len(digest) // 2]
    return base64.urlsafe_b64encode(left_half).rstrip(b"=").decode("ascii")


def _verify_token_identity_binding(access_claims: dict, id_claims: dict, at_hash_verified: bool) -> bool:
    access_iss = access_claims.get("iss")
    id_iss = id_claims.get("iss")
    if access_iss is not None and id_iss is not None and access_iss != id_iss:
        return False

    access_oid = access_claims.get("oid")
    id_oid = id_claims.get("oid")
    if access_oid is not None or id_oid is not None:
        return bool(access_oid) and access_oid == id_oid

    access_sub = access_claims.get("sub")
    id_sub = id_claims.get("sub")
    if access_sub is not None and id_sub is not None:
        return bool(access_sub) and access_sub == id_sub

    if access_sub is not None and id_sub is None:
        return False

    return at_hash_verified


def validate_jwt_token(token: str, scheme: str, config: dict, token_type: str):
    jwks_uri = None
    if scheme == "oidc-entra-id":
        jwks_uri = f"https://login.microsoftonline.com/{config['tenant_id']}/discovery/v2.0/keys"
    elif scheme == "oidc-okta":
        jwks_uri = f"{config['id_issuer']}/v1/keys"
    elif scheme == "oidc-custom":
        jwks_uri = f"{config['public_key_endpoint']}"

    try:
        response = requests.get(jwks_uri, timeout=10, verify=config.get("cert", True))
        response.raise_for_status()
        jwks = response.json()
    except (requests.RequestException, ValueError) as e:
        return {"valid": False, "errors": f"Failed to fetch JWKS: {str(e)}"}

    expected_issuer = _expected_issuer(scheme, config, token_type)
    if not expected_issuer:
        return {"valid": False, "errors": "Missing expected token issuer configuration"}

    try:
        header = jwt.get_unverified_header(token)  # NOSONAR
    except Exception as e:
        return {"valid": False, "errors": f"Failed to decode JWT header: {str(e)}"}

    if "alg" not in header or header["alg"] != "RS256":
        return {"valid": False, "errors": "Invalid algorithm in header"}

    if "kid" not in header:
        return {"valid": False, "errors": "Missing key ID (kid) in JWT header"}

    kid = header["kid"]

    if "keys" not in jwks or not isinstance(jwks["keys"], list):
        return {"valid": False, "errors": "Invalid JWKS response: missing or invalid keys array"}

    decoded_token = None
    for jwk in jwks["keys"]:
        if kid != jwk["kid"]:
            continue
        try:
            public_key = jwt.algorithms.RSAAlgorithm.from_jwk(json.dumps(jwk))
            if scheme == "oidc-custom" and token_type == "access":
                expected_aud = config["access_token_audience"]
            elif scheme == "oidc-custom" and token_type == "id":
                expected_aud = config["id_token_audience"]
            elif scheme == "oidc-okta" and token_type == "access":
                expected_aud = config["authorization_server"]
            else:
                expected_aud = config["client_id"]
            decoded_token = jwt.decode(
                token,
                key=public_key,
                algorithms=["RS256"],
                audience=expected_aud,
                issuer=expected_issuer,
                options={"require": ["exp", "iss", "aud"]},
            )
        except Exception as e:
            return {"valid": False, "errors": " ".join(str(arg) for arg in e.args)}

    if decoded_token is None:
        return {"valid": False, "errors": "Matching kid not found"}

    if decoded_token.get("iss") != expected_issuer:
        return {"valid": False, "errors": "Token issuer mismatch"}

    exp = decoded_token.get("exp")
    if exp is None:
        return {"valid": False, "errors": "Token missing expiration claim"}

    try:
        if int(exp) < int(time.time()):
            return {"valid": False, "errors": "Token expired"}
    except (ValueError, TypeError):
        return {"valid": False, "errors": "Invalid expiration claim in token"}

    return {"valid": True, "token": decoded_token}


def _parse_response(response):
    return dict(token.split("=", 1) for token in response.split(";"))


def _load_role_mappings(raw_role_mappings: str) -> dict:
    if raw_role_mappings:
        role_mapping = {}
        raw_role_mappings = raw_role_mappings.strip().split(";")
        for mapping in raw_role_mappings:
            if not mapping.strip():
                continue
            mapping_list = mapping.split(":")
            if len(mapping_list) != 2:
                raise ValueError(f"Invalid role mapping: {mapping}")
            idp_role, mg_roles = mapping_list
            roles = [role.strip() for role in mg_roles.split(",") if role.strip()]
            if not roles:
                raise ValueError(f"No valid roles specified for: {idp_role}")
            role_mapping[idp_role.strip()] = roles
        return role_mapping

    raise ValueError("Missing role mappings")


def _load_config_from_env(scheme: str):
    config = {}

    if scheme == "oidc-entra-id":
        config["client_id"] = os.environ.get("MEMGRAPH_SSO_ENTRA_ID_OIDC_CLIENT_ID", "")
        config["tenant_id"] = os.environ.get("MEMGRAPH_SSO_ENTRA_ID_OIDC_TENANT_ID", "")
        config["issuer"] = os.environ.get("MEMGRAPH_SSO_ENTRA_ID_OIDC_ISSUER", "")
        config["role_field"] = os.environ.get("MEMGRAPH_SSO_ENTRA_ID_OIDC_ROLE_FIELD", "roles")
        config["username"] = os.environ.get("MEMGRAPH_SSO_ENTRA_ID_OIDC_USERNAME", "id:sub")
        config["role_mapping"] = _load_role_mappings(os.environ.get("MEMGRAPH_SSO_ENTRA_ID_OIDC_ROLE_MAPPING", ""))
    elif scheme == "oidc-okta":
        config["client_id"] = os.environ.get("MEMGRAPH_SSO_OKTA_OIDC_CLIENT_ID", "")
        config["id_issuer"] = os.environ.get("MEMGRAPH_SSO_OKTA_OIDC_ISSUER", "")
        config["authorization_server"] = os.environ.get("MEMGRAPH_SSO_OKTA_OIDC_AUTHORIZATION_SERVER", "")
        config["role_field"] = os.environ.get("MEMGRAPH_SSO_OKTA_OIDC_ROLE_FIELD", "groups")
        config["username"] = os.environ.get("MEMGRAPH_SSO_OKTA_OIDC_USERNAME", "id:sub")
        config["role_mapping"] = _load_role_mappings(os.environ.get("MEMGRAPH_SSO_OKTA_OIDC_ROLE_MAPPING", ""))
    elif scheme == "oidc-custom":
        config["public_key_endpoint"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_PUBLIC_KEY_ENDPOINT", "")
        config["issuer"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ISSUER", "")
        config["access_token_issuer"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ACCESS_TOKEN_ISSUER", "")
        config["id_token_issuer"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ID_TOKEN_ISSUER", "")
        config["access_token_audience"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ACCESS_TOKEN_AUDIENCE", "")
        config["id_token_audience"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ID_TOKEN_AUDIENCE", "")
        config["role_field"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ROLE_FIELD", "")
        config["username"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_USERNAME", "")
        config["role_mapping"] = _load_role_mappings(os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_ROLE_MAPPING", ""))
        config["cert"] = os.environ.get("MEMGRAPH_SSO_CUSTOM_OIDC_EXTRA_CA_CERTS", True)
    config["use_id_token"] = config["username"].startswith("id:")
    return config


def decode_tokens(scheme: str, config: dict, tokens: dict):
    if config["use_id_token"]:
        access_res = validate_jwt_token(tokens["access_token"], scheme, config, "access")
        id_res = validate_jwt_token(tokens["id_token"], scheme, config, "id")
        if access_res.get("valid") and id_res.get("valid"):
            id_claims = id_res["token"]
            if "at_hash" in id_claims:
                try:
                    expected_at_hash = _compute_at_hash(tokens["access_token"])
                except UnicodeEncodeError:
                    return (access_res, {"valid": False, "errors": "Invalid access token encoding for at_hash check"})
                if not hmac.compare_digest(str(id_claims["at_hash"]), expected_at_hash):
                    return (access_res, {"valid": False, "errors": "ID token at_hash does not match access token"})
                id_res["at_hash_verified"] = True
        return (access_res, id_res)

    return (validate_jwt_token(tokens["access_token"], scheme, config, "access"),)


def process_tokens(tokens: tuple, config: dict, scheme: str):
    access_entry = tokens[0]
    id_entry = tokens[1] if config["use_id_token"] else None

    if "errors" in access_entry:
        return {"authenticated": False, "errors": f"Error while decoding access token: {access_entry['errors']}"}
    if id_entry is not None and "errors" in id_entry:
        return {"authenticated": False, "errors": f"Error while decoding id token: {id_entry['errors']}"}

    access_token = access_entry["token"]
    id_token = id_entry["token"] if id_entry is not None else None
    at_hash_verified = bool(id_entry.get("at_hash_verified", False)) if id_entry is not None else False
    roles_field = config["role_field"]

    if roles_field not in access_token:
        return {
            "authenticated": False,
            "errors": f"Missing roles field named {roles_field}, roles are probably not correctly configured on the token issuer",
        }

    roles = []
    idp_roles = access_token[roles_field]
    if isinstance(idp_roles, list):
        matching_roles = set()

        for idp_role in idp_roles:
            if idp_role in config["role_mapping"]:
                matching_roles.update(config["role_mapping"][idp_role])

        if not matching_roles:
            return {
                "authenticated": False,
                "errors": f"Cannot map any of the roles {sorted(idp_roles)} to Memgraph roles",
            }
        roles = list(matching_roles)
    elif isinstance(idp_roles, str):
        if idp_roles not in config["role_mapping"]:
            return {"authenticated": False, "errors": f"Cannot map role {idp_roles} to Memgraph role"}
        roles = config["role_mapping"][idp_roles]

    try:
        token_type, field = config["username"].split(":")
    except ValueError:
        return {
            "authenticated": False,
            "errors": f"Invalid username configuration format: {config['username']}. Expected format: 'token_type:field_name'",
        }

    if (token_type == "id" and field not in id_token) or (token_type == "access" and field not in access_token):
        return {"authenticated": False, "errors": f"Field {field} missing in {token_type} token"}

    if token_type == "id" and id_token is not None:
        if not _verify_token_identity_binding(access_token, id_token, at_hash_verified):
            return {
                "authenticated": False,
                "errors": "Access token and ID token do not belong to the same identity",
            }

    return {
        "authenticated": True,
        "roles": roles,
        "username": id_token[field] if token_type == "id" else access_token[field],
    }


def authenticate(response: str, scheme: str):
    if scheme not in ["oidc-entra-id", "oidc-okta", "oidc-custom"]:
        return {"authenticated": False, "errors": "Invalid SSO scheme"}

    try:
        config = _load_config_from_env(scheme)
        tokens = _parse_response(response)
        return process_tokens(decode_tokens(scheme, config, tokens), config, scheme)
    except Exception as e:
        return {"authenticated": False, "errors": f"Error: {' '.join(str(arg) for arg in e.args)}"}


def process_pipe_line(raw_line: bytes) -> bytes:
    from common import add_call_id_to_response, pop_call_id

    call_id = None
    try:
        params = json.loads(raw_line.decode("utf-8"))
        if not isinstance(params, dict):
            raise ValueError("Request payload must be a JSON object")
        call_id = pop_call_id(params)
        ret = authenticate(**params)
    except Exception as exc:
        ret = {"authenticated": False, "errors": f"Invalid authentication request: {exc}"}
    add_call_id_to_response(ret, call_id)
    return (json.dumps(ret) + "\n").encode("utf-8")


if __name__ == "__main__":
    input_stream = io.FileIO(1000, mode="r")
    output_stream = io.FileIO(1001, mode="w")
    while True:
        line = input_stream.readline()
        if not line:
            break
        output_stream.write(process_pipe_line(line))
        output_stream.flush()
