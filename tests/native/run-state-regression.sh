#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/aap-lv2-state.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
for config in Debug Release; do
    cmake -S "$script_dir" -B "$build_dir/$config" -DCMAKE_BUILD_TYPE="$config" -DSANITIZERS="${SANITIZERS:-undefined}"
    cmake --build "$build_dir/$config" --parallel
    ctest --test-dir "$build_dir/$config" --output-on-failure
done
