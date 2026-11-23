#!/bin/bash -e
COLOR_ORANGE="\e[38;5;208m"
COLOR_GREEN="\e[38;5;35m"
COLOR_RED="\e[0;31m"
COLOR_NULL="\e[0m"

print_help() {
    echo -e "${COLOR_ORANGE}HOW TO RUN:${COLOR_NULL} $0 input_file_schema_path input_file_nodes_path input_file_relationships_path input_file_cleanup_path output_file_path"
    exit 1
}

if [ "$#" -ne 5 ]; then
    print_help
fi
INPUT_SCHEMA="$1"
INPUT_NODES="$2"
INPUT_RELATIONSHIPS="$3"
INPUT_CLEANUP="$4"
OUTPUT="$5"

for in_file in "$INPUT_SCHEMA" "$INPUT_NODES" "$INPUT_RELATIONSHIPS" "$INPUT_CLEANUP"; do
    if [ ! -f "$in_file" ]; then
        echo -e "${COLOR_RED}ERROR:${COLOR_NULL} input_file_path is not a file!"
        print_help
    fi
    if [ "$in_file" -ef "$OUTPUT" ]; then
        echo -e "${COLOR_RED}ERROR:${COLOR_NULL} output_file_path must not refer to any input file!"
        exit 1
    fi
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

    cat "$INPUT_NODES"
    cat "$INPUT_RELATIONSHIPS"

    sed -e '/^[[:space:]]*DROP CONSTRAINT/d' "$INPUT_CLEANUP"
    echo 'DROP INDEX ON :`UNIQUE IMPORT LABEL`(`UNIQUE IMPORT ID`);'
} > "$OUTPUT"

echo ""
echo -e "${COLOR_GREEN}DONE!${COLOR_NULL} Please find Memgraph compatible cypherl|.cypher file under $OUTPUT"
echo ""
echo "Please import data by executing => \`cat $OUTPUT | mgconsole\`"
