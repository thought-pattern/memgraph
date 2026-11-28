#!/bin/bash -e
COLOR_ORANGE="\e[38;5;208m"
COLOR_GREEN="\e[38;5;35m"
COLOR_RED="\e[0;31m"
COLOR_NULL="\e[0m"

print_help() {
    echo -e "${COLOR_ORANGE}HOW TO RUN:${COLOR_NULL} $0 memgraph_logs_file_path cypherl_output_path"
    exit 1
}

if [ "$#" -ne 2 ]; then
    print_help
fi
INPUT="$1"
OUTPUT="$2"
if [ ! -f "$INPUT" ]; then
    echo -e "${COLOR_RED}ERROR:${COLOR_NULL} memgraph_logs_file_path is not a file!"
    print_help
fi
if [ "$INPUT" -ef "$OUTPUT" ]; then
    echo -e "${COLOR_RED}ERROR:${COLOR_NULL} cypherl_output_path must not refer to the same file as memgraph_logs_file_path!"
    exit 1
fi

awk '
function ends_with_closed_query(text,    tmp) {
    tmp = text
    sub(/[[:space:]]*$/, "", tmp)
    if (tmp ~ /\047 - \{[^\047]*\}$/) {
        return 1
    }
    return (tmp ~ /\047$/)
}
function flush_query(    q) {
    if (!in_run) {
        return
    }
    q = buf
    sub(/[[:space:]]*$/, "", q)
    if (q ~ /\047 - \{[^\047]*\}$/) {
        sub(/\047 - \{[^\047]*\}$/, "", q)
    } else {
        sub(/\047$/, "", q)
    }
    if (q !~ /;[[:space:]]*$/) {
        q = q ";"
    }
    print q
    in_run = 0
    buf = ""
}
{
    line = $0
    sub(/\r$/, "", line)
    if (match(line, /\[Run( - [^\]]+)?\] \047/)) {
        flush_query()
        in_run = 1
        buf = substr(line, RSTART + RLENGTH)
    } else if (in_run) {
        if (line ~ /^\[/ && ends_with_closed_query(buf)) {
            flush_query()
        } else {
            buf = buf "\n" line
        }
    }
}
END {
    flush_query()
}
' < "$INPUT" > "$OUTPUT"

echo -e "${COLOR_GREEN}DONE!${COLOR_NULL} Please find Memgraph compatible cypherl file under $OUTPUT"
echo ""
echo "Import can be done by executing => \`cat $OUTPUT | mgconsole\`"
