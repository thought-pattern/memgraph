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


def parse(el, ignore_order):
    """
    Function used to parse result element. Result element can be
    node, list, path, relationship, map or string. Function returns
    same string for two same elements, etc. if properties of two nodes
    are inverted, but labels and properties are the same, function
    will return the same string for them. For two differenet
    elements function will not return the same strings.

    @param el:
        string, element to parse.
    @param ignore_order:
        bool, ignore order inside of lists, etc. lists [1, 2, 3] and
        [2, 3, 1] are the same if ignore_order is true, else false.
    @return:
        Parsed string of element.
    """
    if el.startswith("(") and el.endswith(")"):
        return parse_node(el, ignore_order)
    if el.startswith("<") and el.endswith(">"):
        return parse_path(el, ignore_order)
    if el.startswith("{") and el.endswith("}"):
        return parse_map(el, ignore_order)
    if el.startswith("[") and el.endswith("]"):
        if is_list(el):
            return parse_list(el, ignore_order)
        else:
            return parse_rel(el, ignore_order)
    return el


def is_list(el):
    """
    Function returns true if string el is a list, else false.
    @param el:
        string, element to check.
    @return:
        true if el is list.
    """
    if len(el) > 1 and el[1] == ":":
        return False
    return True


def _advance_quote_state(ch, quote_char, escaped):
    if quote_char is not None:
        if escaped:
            return quote_char, False
        if ch == "\\":
            return quote_char, True
        if ch == quote_char:
            return None, False
        return quote_char, False
    if ch in ("'", '"'):
        return ch, False
    return None, False


def parse_path(path, ignore_order):
    """
    Function used to parse path.
    @param path:
        string representing path
    @return:
        parsed path
    """
    parsed_path = "<"
    dif_open_closed_brackets = 0
    quote_char = None
    escaped = False
    start = 1
    for i in range(1, len(path) - 1):
        was_quoted = quote_char is not None
        quote_char, escaped = _advance_quote_state(path[i], quote_char, escaped)
        if was_quoted or quote_char is not None:
            continue
        if path[i] in ("(", "{", "["):
            dif_open_closed_brackets += 1
            if dif_open_closed_brackets == 1:
                start = i
        if path[i] in (")", "}", "]"):
            dif_open_closed_brackets -= 1
            if dif_open_closed_brackets == 0:
                parsed_path += parse(path[start : (i + 1)], ignore_order)
        elif dif_open_closed_brackets == 0:
            parsed_path += path[i]
    parsed_path += ">"
    return parsed_path


def parse_node(node_str, ignore_order):
    """
    Function used to parse node.
    @param node:
        string representing node
    @return:
        parsed node
    """
    label = ""
    labels = []
    props_start = None
    for i in range(1, len(node_str)):
        if node_str[i] in (":", ")", "{"):
            if label.startswith(":"):
                labels.append(label)
                label = ""
        label += node_str[i]

        if node_str[i] == "{":
            props_start = i
            break

    labels.sort()
    parsed_node = "("
    for label in labels:
        parsed_node += label
    if props_start is not None:
        parsed_node += parse_map(node_str[props_start : len(node_str) - 1], ignore_order)
    parsed_node += ")"
    return parsed_node


def parse_map(props, ignore_order):
    """
    Function used to parse map.
    @param props:
        string representing map
    @return:
        parsed map
    """
    dif_open_closed_brackets = 0
    quote_char = None
    escaped = False
    prop = ""
    list_props = []
    for i in range(1, len(props) - 1):
        ch = props[i]
        was_quoted = quote_char is not None
        quote_char, escaped = _advance_quote_state(ch, quote_char, escaped)
        in_quotes = was_quoted or quote_char is not None
        if not in_quotes and ch == "," and dif_open_closed_brackets == 0:
            list_props.append(prop_to_str(prop, ignore_order))
            prop = ""
        else:
            prop += ch
        if not in_quotes:
            if ch in ("(", "{", "["):
                dif_open_closed_brackets += 1
            elif ch in (")", "}", "]"):
                dif_open_closed_brackets -= 1
    if prop != "":
        list_props.append(prop_to_str(prop, ignore_order))

    list_props.sort()
    return "{" + ",".join(list_props) + "}"


def prop_to_str(prop, ignore_order):
    """
    Function used to parse one pair of key, value in format 'key:value'.
    Value must be parsed, key is string.

    @param prop:
        string, pair key:value to parse
    @return:
        parsed prop
    """
    quote_char = None
    escaped = False
    for idx, ch in enumerate(prop):
        was_quoted = quote_char is not None
        quote_char, escaped = _advance_quote_state(ch, quote_char, escaped)
        if not was_quoted and quote_char is None and ch == ":":
            key = prop[:idx]
            val = prop[idx + 1 :]
            return key + ":" + parse(val, ignore_order)
    key, val = prop.split(":", 1)
    return key + ":" + parse(val, ignore_order)


def parse_list(l, ignore_order):
    """
    Function used to parse list.
    @param l:
        string representing list
    @return:
        parsed list
    """
    dif_open_closed_brackets = 0
    quote_char = None
    escaped = False
    el = ""
    list_el = []
    for i in range(1, len(l) - 1):
        ch = l[i]
        was_quoted = quote_char is not None
        quote_char, escaped = _advance_quote_state(ch, quote_char, escaped)
        in_quotes = was_quoted or quote_char is not None
        if not in_quotes and ch == "," and dif_open_closed_brackets == 0:
            list_el.append(parse(el, ignore_order))
            el = ""
        else:
            el += ch
        if not in_quotes:
            if ch in ("(", "{", "["):
                dif_open_closed_brackets += 1
            elif ch in (")", "}", "]"):
                dif_open_closed_brackets -= 1
    if el != "":
        list_el.append(parse(el, ignore_order))

    if ignore_order:
        list_el.sort()

    return "[" + ",".join(list_el) + "]"


def parse_rel(rel, ignore_order):
    """
    Function used to parse relationship.
    @param rel:
        string representing relationship
    @return:
        parsed relationship
    """
    label = ""
    labels = []
    props_start = None
    for i in range(1, len(rel)):
        if rel[i] in (":", "]", "{"):
            if label.startswith(":"):
                labels.append(label)
                label = ""
        label += rel[i]

        if rel[i] == "{":
            props_start = i
            break

    labels.sort()
    parsed_rel = "["
    for label in labels:
        parsed_rel += label
    if props_start is not None:
        parsed_rel += parse_map(rel[props_start : len(rel) - 1], ignore_order)
    parsed_rel += "]"
    return parsed_rel
