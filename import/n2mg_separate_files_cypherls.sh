#!/bin/bash -e
COLOR_ORANGE="\e[38;5;208m"
COLOR_GREEN="\e[38;5;35m"
COLOR_RED="\e[0;31m"
COLOR_NULL="\e[0m"

print_help() {
    echo -e "${COLOR_ORANGE}HOW TO RUN:${COLOR_NULL} $0 input_file_schema_path input_file_nodes_path input_file_relationships_path input_file_cleanup_path output_file_schema_path output_file_nodes_path output_file_relationships_path output_file_cleanup_path"
    exit 1
}

if [ "$#" -ne 8 ]; then
    print_help
fi
INPUT_SCHEMA="$1"
INPUT_NODES="$2"
INPUT_RELATIONSHIPS="$3"
INPUT_CLEANUP="$4"
OUTPUT_SCHEMA="$5"
OUTPUT_NODES="$6"
OUTPUT_RELATIONSHIPS="$7"
OUTPUT_CLEANUP="$8"

INPUTS=("$INPUT_SCHEMA" "$INPUT_NODES" "$INPUT_RELATIONSHIPS" "$INPUT_CLEANUP")
OUTPUTS=("$OUTPUT_SCHEMA" "$OUTPUT_NODES" "$OUTPUT_RELATIONSHIPS" "$OUTPUT_CLEANUP")

for in_file in "${INPUTS[@]}"; do
    if [ ! -f "$in_file" ]; then
        echo -e "${COLOR_RED}ERROR:${COLOR_NULL} input_file_path is not a file!"
        print_help
    fi
    for out_file in "${OUTPUTS[@]}"; do
        if [ "$in_file" -ef "$out_file" ]; then
            echo -e "${COLOR_RED}ERROR:${COLOR_NULL} output files must not refer to any input file!"
            exit 1
        fi
    done
done

canonical_out_path() {
    local target="$1"
    local dir
    dir="$(cd "$(dirname "$target")" && pwd -P)"
    printf '%s/%s\n' "$dir" "$(basename "$target")"
}

for ((i = 0; i < ${#OUTPUTS[@]}; i++)); do
    for ((j = i + 1; j < ${#OUTPUTS[@]}; j++)); do
        if [ "${OUTPUTS[$i]}" -ef "${OUTPUTS[$j]}" ] || [ "$(canonical_out_path "${OUTPUTS[$i]}")" = "$(canonical_out_path "${OUTPUTS[$j]}")" ]; then
            echo -e "${COLOR_RED}ERROR:${COLOR_NULL} output file paths must be distinct!"
            exit 1
        fi
    done
done

echo -e "${COLOR_ORANGE}NOTE:${COLOR_NULL} BEGIN and COMMIT are required because variables share the same name (e.g. row)"
echo -e "${COLOR_ORANGE}NOTE:${COLOR_NULL} CONSTRAINTS are just skipped -> ${COLOR_RED}please create constraints manually if needed${COLOR_NULL}"

{
    echo 'CREATE INDEX ON :`UNIQUE IMPORT LABEL`(`UNIQUE IMPORT ID`);'
    awk '
    function trim(s) {
        sub(/^[[:space:]]+/, "", s)
        sub(/[[:space:]]+$/, "", s)
        return s
    }
    function rewrite_index_stmt(stmt,    semi, body, upper_body, tail, var_label, rest, colon_pos, var_name, label_name, props_raw, n, i, ch, in_bt, cur, out_props, prefix) {
        semi = ""
        body = stmt
        if (body ~ /;[[:space:]]*$/) {
            semi = ";"
            sub(/;[[:space:]]*$/, "", body)
        }
        upper_body = toupper(body)
        if (upper_body !~ /^[[:space:]]*CREATE[[:space:]]+((RANGE|BTREE)[[:space:]]+)?INDEX([[:space:]]|$)/) {
            return stmt
        }
        if (!match(body, /[Ff][Oo][Rr][[:space:]]*\([^)]*\)[[:space:]]*[Oo][Nn][[:space:]]*\([^)]*\)[[:space:]]*$/)) {
            return stmt
        }
        tail = substr(body, RSTART, RLENGTH)
        if (!match(tail, /\([^)]*\)/)) {
            return stmt
        }
        var_label = substr(tail, RSTART + 1, RLENGTH - 2)
        rest = substr(tail, RSTART + RLENGTH)
        colon_pos = index(var_label, ":")
        if (colon_pos <= 0) {
            return stmt
        }
        var_name = trim(substr(var_label, 1, colon_pos - 1))
        label_name = trim(substr(var_label, colon_pos + 1))
        if (!match(rest, /\([^)]*\)/)) {
            return stmt
        }
        props_raw = substr(rest, RSTART + 1, RLENGTH - 2)
        n = length(props_raw)
        in_bt = 0
        cur = ""
        out_props = ""
        prefix = var_name "."
        for (i = 1; i <= n; i++) {
            ch = substr(props_raw, i, 1)
            if (ch == "`") {
                in_bt = !in_bt
                cur = cur ch
            } else if (ch == "," && !in_bt) {
                cur = trim(cur)
                if (index(cur, prefix) == 1) {
                    cur = substr(cur, length(prefix) + 1)
                }
                out_props = (out_props == "" ? cur : out_props ", " cur)
                cur = ""
            } else {
                cur = cur ch
            }
        }
        cur = trim(cur)
        if (cur != "") {
            if (index(cur, prefix) == 1) {
                cur = substr(cur, length(prefix) + 1)
            }
            out_props = (out_props == "" ? cur : out_props ", " cur)
        }
        return "CREATE INDEX ON :" label_name "(" out_props ")" semi
    }
    {
        line = $0
        trimmed = trim(line)
        upper_s = toupper(trimmed)
        if (upper_s ~ /^CREATE[[:space:]]+CONSTRAINT([[:space:];]|$)/) {
            next
        }
        if (upper_s ~ /^SCHEMA[[:space:]]+AWAIT([[:space:];]|$)/) {
            next
        }
        if (upper_s ~ /^CALL[[:space:]]+DB\.AWAITINDEX(ES)?[[:space:]]*(\(|;|$)/) {
            next
        }
        print rewrite_index_stmt(line)
    }
    ' "$INPUT_SCHEMA"
} > "$OUTPUT_SCHEMA"

awk '
BEGIN {
    depth = 0
    in_sq = 0
    in_dq = 0
    in_bt = 0
    in_bc = 0
}
{
    line = $0
    n = length(line)
    out = ""
    i = 1
    while (i <= n) {
        ch = substr(line, i, 1)
        nx = (i < n ? substr(line, i + 1, 1) : "")
        if (in_bc) {
            if (ch == "*" && nx == "/") {
                out = out "*/"
                in_bc = 0
                i += 2
            } else {
                out = out ch
                i++
            }
        } else if (in_sq) {
            out = out ch
            if (ch == "\\" && i < n) {
                i++
                out = out substr(line, i, 1)
            } else if (ch == "\047") {
                if (nx == "\047") {
                    i++
                    out = out "\047"
                } else {
                    in_sq = 0
                }
            }
            i++
        } else if (in_dq) {
            out = out ch
            if (ch == "\\" && i < n) {
                i++
                out = out substr(line, i, 1)
            } else if (ch == "\"") {
                in_dq = 0
            }
            i++
        } else if (in_bt) {
            out = out ch
            if (ch == "`") {
                if (nx == "`") {
                    i++
                    out = out "`"
                } else {
                    in_bt = 0
                }
            }
            i++
        } else {
            if ((ch == "/" && nx == "/") || (ch == "-" && nx == "-")) {
                out = out substr(line, i)
                break
            } else if (ch == "/" && nx == "*") {
                in_bc = 1
                out = out "/*"
                i += 2
            } else if (ch == "\047") {
                if (depth > 0) expect_key[depth] = 0
                in_sq = 1
                out = out ch
                i++
            } else if (ch == "\"") {
                if (depth > 0) expect_key[depth] = 0
                in_dq = 1
                out = out ch
                i++
            } else if (ch == "`") {
                if (depth > 0) expect_key[depth] = 0
                in_bt = 1
                out = out ch
                i++
            } else if (ch == "{") {
                if (depth > 0) expect_key[depth] = 0
                depth++
                stack[depth] = "{"
                expect_key[depth] = 1
                out = out ch
                i++
            } else if (ch == "[" || ch == "(") {
                if (depth > 0) expect_key[depth] = 0
                depth++
                stack[depth] = ch
                expect_key[depth] = 0
                out = out ch
                i++
            } else if (ch == "}" || ch == "]" || ch == ")") {
                if (depth > 0) depth--
                out = out ch
                i++
            } else if (ch == ",") {
                if (depth > 0 && stack[depth] == "{") {
                    expect_key[depth] = 1
                }
                out = out ch
                i++
            } else if (ch ~ /[[:space:]]/) {
                out = out ch
                i++
            } else if (ch ~ /[A-Za-z_]/) {
                j = i + 1
                while (j <= n && substr(line, j, 1) ~ /[A-Za-z0-9_]/) {
                    j++
                }
                ident = substr(line, i, j - i)
                k = j
                while (k <= n && substr(line, k, 1) ~ /[[:space:]]/) {
                    k++
                }
                after_ws = (k <= n ? substr(line, k, 1) : "")
                after_ws2 = (k < n ? substr(line, k + 1, 1) : "")
                if (depth > 0 && stack[depth] == "{" && expect_key[depth] == 1 && after_ws == ":" && after_ws2 != ":") {
                    out = out "`" ident "`"
                } else {
                    out = out ident
                }
                if (depth > 0) expect_key[depth] = 0
                i = j
            } else {
                if (depth > 0) expect_key[depth] = 0
                out = out ch
                i++
            }
        }
    }
    print out
}
' "$INPUT_NODES" > "$OUTPUT_NODES"

cat "$INPUT_RELATIONSHIPS" > "$OUTPUT_RELATIONSHIPS"

{
    sed -e '/^[[:space:]]*DROP CONSTRAINT/d' "$INPUT_CLEANUP"
    echo 'DROP INDEX ON :`UNIQUE IMPORT LABEL`(`UNIQUE IMPORT ID`);'
} > "$OUTPUT_CLEANUP"

echo ""
echo -e "${COLOR_GREEN}DONE!${COLOR_NULL} Please find Memgraph compatible cypherl|.cypher files under $OUTPUT_SCHEMA, $OUTPUT_NODES, $OUTPUT_RELATIONSHIPS and $OUTPUT_CLEANUP"
echo ""
echo "Please import data by executing => \`cat $OUTPUT_SCHEMA | mgconsole\`, \`cat $OUTPUT_NODES | mgconsole\`, \`cat $OUTPUT_RELATIONSHIPS | mgconsole\` and \`cat $OUTPUT_CLEANUP | mgconsole\`"
