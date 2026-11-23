#!/usr/bin/env python3
import argparse
import copy
import os
import re
import shlex
import subprocess
import sys
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

import yaml

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
CONFIG_FILE = os.path.join(SCRIPT_DIR, "flags.yaml")
DEFAULT_SOURCE_DIR = os.path.join(os.path.dirname(SCRIPT_DIR), "src")
WIDTH = 80


def wrap_text(s, initial_indent="# "):
    return "\n#\n".join(
        map(lambda x: textwrap.fill(x, WIDTH, initial_indent=initial_indent, subsequent_indent="# "), s.split("\n"))
    )


def unescape_cpp_string(raw):
    out = []
    idx = 0
    length = len(raw)
    escapes = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'"}
    while idx < length:
        if raw[idx] == "\\" and idx + 1 < length:
            nxt = raw[idx + 1]
            out.append(escapes.get(nxt, nxt))
            idx += 2
        else:
            out.append(raw[idx])
            idx += 1
    return "".join(out)


def parse_cpp_string_literals(expr, file_text=""):
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', expr)
    if parts:
        return "".join(unescape_cpp_string(raw) for raw in parts)
    ident_m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\.c_str\(\))?$", expr.strip())
    if ident_m and file_text:
        ident = ident_m.group(1)
        decl_m = re.search(rf"\b{re.escape(ident)}\b\s*=\s*((?:\"[^\"]*\"\s*)+);", file_text)
        if decl_m:
            nested = re.findall(r'"((?:[^"\\]|\\.)*)"', decl_m.group(1))
            if nested:
                return "".join(unescape_cpp_string(raw) for raw in nested)
    return expr.strip()


def split_macro_args(arg_str):
    args = []
    current = []
    depth = 0
    in_str = False
    escape = False
    for ch in arg_str:
        if in_str:
            current.append(ch)
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            current.append(ch)
        elif ch in "([{":
            depth += 1
            current.append(ch)
        elif ch in ")]}":
            depth -= 1
            current.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        args.append("".join(current).strip())
    return args


def normalize_default_value(flag_type, raw_expr):
    cleaned = raw_expr.strip()
    if flag_type == "bool":
        if cleaned in ("true", "1"):
            return "true"
        if cleaned in ("false", "0"):
            return "false"
        return cleaned.lower()
    if flag_type == "string":
        if '"' in cleaned:
            return parse_cpp_string_literals(cleaned)
        return ""
    # Strip C++ integer/float literal suffixes and digit separators (e.g. 100'000ULL, 0.5f).
    cleaned = cleaned.replace("'", "")
    num_match = re.match(r"^([+-]?(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?))[uUlLfF]*$", cleaned)
    if num_match:
        token = num_match.group(1)
        if token.lower().startswith("0x"):
            return str(int(token, 16))
        return token
    return "0.0" if flag_type == "double" else "0"


def extract_flags_from_sources(source_dir):
    ret = {
        "flag_file": {"name": "flag_file", "meaning": "load flags from file", "default": "", "type": "string", "override": False},
        "help": {"name": "help", "meaning": "show help on all flags", "default": "false", "type": "bool", "override": False},
        "help_xml": {"name": "help_xml", "meaning": "produce an xml version of help", "default": "false", "type": "bool", "override": False},
        "version": {"name": "version", "meaning": "show version and build info and exit", "default": "false", "type": "bool", "override": False},
    }
    macro_re = re.compile(r"\bDEFINE_(?:VALIDATED_)?(?:HIDDEN_)?(bool|int32|int64|uint32|uint64|double|string)\s*\(")
    src_path = Path(source_dir)
    if not src_path.is_dir():
        raise RuntimeError(f"Cannot extract flags statically: source directory '{source_dir}' does not exist")

    for path in sorted(src_path.rglob("*")):
        if path.suffix not in (".cpp", ".cc", ".cxx", ".hpp", ".h"):
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        pos = 0
        while True:
            match = macro_re.search(text, pos)
            if not match:
                break
            flag_type = match.group(1)
            open_paren = match.end() - 1
            depth = 0
            in_str = False
            escape = False
            end_paren = -1
            for idx in range(open_paren, len(text)):
                ch = text[idx]
                if in_str:
                    if escape:
                        escape = False
                    elif ch == "\\":
                        escape = True
                    elif ch == '"':
                        in_str = False
                    continue
                if ch == '"':
                    in_str = True
                elif ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        end_paren = idx
                        break
            if end_paren < 0:
                break
            pos = end_paren + 1
            macro_args = split_macro_args(text[open_paren + 1 : end_paren])
            if len(macro_args) < 3:
                continue
            name_tok, default_tok, help_tok = macro_args[:3]
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name_tok):
                continue
            ret[name_tok] = {
                "name": name_tok,
                "meaning": parse_cpp_string_literals(help_tok, text),
                "default": normalize_default_value(flag_type, default_tok),
                "type": flag_type,
                "override": False,
            }
    return ret


def extract_flags(binary_path, source_dir=DEFAULT_SOURCE_DIR, no_exec=False, emulator=""):
    if no_exec:
        return extract_flags_from_sources(source_dir)

    cmd = shlex.split(emulator) + [binary_path, "--help-xml"] if emulator else [binary_path, "--help-xml"]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError:
        if source_dir and os.path.isdir(source_dir):
            return extract_flags_from_sources(source_dir)
        raise

    raw_out = proc.stdout.decode("utf-8", errors="replace")
    xml_lines = [line for line in raw_out.split("\n") if line.startswith("<")]
    if not xml_lines:
        if source_dir and os.path.isdir(source_dir):
            return extract_flags_from_sources(source_dir)
        raise RuntimeError(f"Failed to extract --help-xml output from '{binary_path}' (rc={proc.returncode})")

    ret = {}
    data = "\n".join(xml_lines)
    root = ET.fromstring(data)
    for child in root:
        if child.tag == "usage" and child.text and child.text.lower().count("warning"):
            raise Exception("You should set the usage message!")
        if child.tag == "flag":
            flag = {}
            for elem in child:
                flag[elem.tag] = elem.text if elem.text is not None else ""
            flag["override"] = False
            ret[flag["name"]] = flag
    return ret


def apply_config_to_flags(config, flags):
    flags = copy.deepcopy(flags)
    for name in config["undocumented"]:
        flags.pop(name, None)
    for modification in config["modifications"]:
        name = modification["name"]
        if name not in flags:
            print("WARNING: Flag '" + name + "' missing from binary!", file=sys.stderr)
            continue
        flags[name]["default"] = modification["value"]
        flags[name]["override"] = modification["override"]
    return flags


def extract_sections(flags):
    sections = []
    other = []
    current_section = ""
    current_flags = []
    for name in sorted(flags.keys()):
        section = name.split("_")[0]
        if section == current_section:
            current_flags.append(name)
        else:
            if len(current_flags) < 2:
                other.extend(current_flags)
            else:
                sections.append((current_section, current_flags))
            current_section = section
            current_flags = [name]
    if len(current_flags) < 2:
        other.extend(current_flags)
    else:
        sections.append((current_section, current_flags))
    sections.append(("other", other))
    assert set(sum(map(lambda x: x[1], sections), [])) == set(
        flags.keys()
    ), "The section extraction algorithm lost some flags!"
    return sections


def generate_config_file(config_spec, sections, flags):
    ret = wrap_text(config_spec["header"]) + "\n\n\n"
    for section, section_flags in sections:
        ret += wrap_text(section.capitalize(), initial_indent="## ") + "\n\n"
        for name in section_flags:
            flag = flags[name]
            helpstr = flag["meaning"] + " [" + flag["type"] + "]"
            ret += wrap_text(helpstr) + "\n"
            prefix = "# " if not flag["override"] else ""
            ret += prefix + "--" + flag["name"].replace("_", "-") + "=" + flag["default"] + "\n\n"
        ret += "\n"
    ret += wrap_text(config_spec["footer"])
    return ret.strip() + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("memgraph_binary", help="path to Memgraph binary")
    parser.add_argument("output_file", help="path where to store the generated Memgraph configuration file")
    parser.add_argument("--config-file", default=CONFIG_FILE, help="path to generator configuration file")
    parser.add_argument("--source-dir", default=DEFAULT_SOURCE_DIR, help="path to Memgraph src directory for cross-builds")
    parser.add_argument("--no-exec", action="store_true", help="extract flags statically without executing target binary")
    parser.add_argument("--emulator", default="", help="optional cross-compiling emulator command")

    args = parser.parse_args()
    flags = extract_flags(
        args.memgraph_binary,
        source_dir=args.source_dir,
        no_exec=args.no_exec,
        emulator=args.emulator,
    )

    with open(args.config_file, encoding="utf-8") as f:
        config = yaml.safe_load(f)

    flags = apply_config_to_flags(config, flags)
    sections = extract_sections(flags)
    data = generate_config_file(config, sections, flags)

    dirname = os.path.dirname(args.output_file)
    if dirname and not os.path.exists(dirname):
        os.makedirs(dirname)

    with open(args.output_file, "w", encoding="utf-8") as f:
        f.write(data)
