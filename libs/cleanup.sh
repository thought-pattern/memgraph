#!/usr/bin/env bash
set -euo pipefail

# go to script directory
working_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$working_dir" || exit 1

# remove archives
shopt -s nullglob
archives=( *.jar *.tar.gz *.tar )
if (( ${#archives[@]} > 0 )); then
    rm -f -- "${archives[@]}"
fi

# remove lib directories
for folder in ./* ; do
    if [ -d "$folder" ]; then
        rm -rf -- "$folder"
    fi
done
