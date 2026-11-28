#!/bin/bash -e
COLOR_ORANGE="\e[38;5;208m"
COLOR_GREEN="\e[38;5;35m"
COLOR_RED="\e[0;31m"
COLOR_NULL="\e[0m"

print_help() {
    echo -e "${COLOR_ORANGE}HOW TO RUN:${COLOR_NULL} $0 input_file_path output_file_path"
    exit 1
}

if [ "$#" -ne 2 ]; then
    print_help
fi
INPUT="$1"
OUTPUT="$2"
if [ ! -f "$INPUT" ]; then
    echo -e "${COLOR_RED}ERROR:${COLOR_NULL} input_file_path is not a file!"
    print_help
fi
if [ "$INPUT" -ef "$OUTPUT" ]; then
    echo -e "${COLOR_RED}ERROR:${COLOR_NULL} output_file_path must not refer to the same file as input_file_path!"
    exit 1
fi

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
    function rewrite_index_stmt(stmt,    semi, body, upper_body, var_label, colon_pos, var_name, label_name, props_raw, n, i, ch, in_bt, cur, out_props, prefix) {
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
    function process_stmt(stmt,    trimmed, upper_s) {
        trimmed = trim(stmt)
        if (trimmed == "") {
            return ""
        }
        upper_s = toupper(trimmed)
        if (upper_s ~ /^:BEGIN[[:space:]]*;?$/ || upper_s == "BEGIN" || upper_s == "BEGIN;") {
            return "BEGIN;"
        }
        if (upper_s ~ /^:COMMIT[[:space:]]*;?$/ || upper_s == "COMMIT" || upper_s == "COMMIT;") {
            return "COMMIT;"
        }
        if (upper_s ~ /^(CREATE|DROP)[[:space:]]+CONSTRAINT([[:space:];]|$)/) {
            return "__SKIP__"
        }
        if (upper_s ~ /^SCHEMA[[:space:]]+AWAIT([[:space:];]|$)/) {
            return "__SKIP__"
        }
        if (upper_s ~ /^CALL[[:space:]]+DB\.AWAITINDEX(ES)?[[:space:]]*(\(|;|$)/) {
            return "__SKIP__"
        }
        return rewrite_index_stmt(stmt)
    }
    {
        line = $0
        if (trim(line) == "") {
            print line
            next
        }
        n = length(line)
        in_sq = 0
        in_dq = 0
        in_bt = 0
        seg = ""
        out_line = ""
        kept_any = 0
        saw_stmt = 0
        for (i = 1; i <= n; i++) {
            ch = substr(line, i, 1)
            if (in_sq) {
                seg = seg ch
                if (ch == "\\") {
                    if (i < n) {
                        i++
                        seg = seg substr(line, i, 1)
                    }
                } else if (ch == "\047") {
                    if (i < n && substr(line, i + 1, 1) == "\047") {
                        i++
                        seg = seg "\047"
                    } else {
                        in_sq = 0
                    }
                }
            } else if (in_dq) {
                seg = seg ch
                if (ch == "\\") {
                    if (i < n) {
                        i++
                        seg = seg substr(line, i, 1)
                    }
                } else if (ch == "\"") {
                    in_dq = 0
                }
            } else if (in_bt) {
                seg = seg ch
                if (ch == "`") {
                    if (i < n && substr(line, i + 1, 1) == "`") {
                        i++
                        seg = seg "`"
                    } else {
                        in_bt = 0
                    }
                }
            } else {
                if (ch == "\047") {
                    in_sq = 1
                    seg = seg ch
                } else if (ch == "\"") {
                    in_dq = 1
                    seg = seg ch
                } else if (ch == "`") {
                    in_bt = 1
                    seg = seg ch
                } else if (ch == ";") {
                    seg = seg ch
                    saw_stmt = 1
                    res = process_stmt(seg)
                    if (res != "__SKIP__" && res != "") {
                        out_line = (kept_any ? out_line " " trim(res) : res)
                        kept_any = 1
                    }
                    seg = ""
                } else {
                    seg = seg ch
                }
            }
        }
        if (trim(seg) != "") {
            saw_stmt = 1
            res = process_stmt(seg)
            if (res != "__SKIP__" && res != "") {
                out_line = (kept_any ? out_line " " trim(res) : res)
                kept_any = 1
            }
        }
        if (kept_any) {
            print out_line
        } else if (!saw_stmt) {
            print line
        }
    }
    ' "$INPUT"
    echo 'DROP INDEX ON :`UNIQUE IMPORT LABEL`(`UNIQUE IMPORT ID`);'
} > "$OUTPUT"

echo ""
echo -e "${COLOR_GREEN}DONE!${COLOR_NULL} Please find Memgraph compatible cypherl|.cypher file under $OUTPUT"
echo ""
echo "Please import data by executing => \`cat $OUTPUT | mgconsole\`"
