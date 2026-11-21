#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build="$root/tests/dna/build"
mkdir -p "$build"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cc=${CC:-gcc}
status=0
for src in tests/dna/t_*.c; do
    [ -e "$src" ] || continue
    name=$(basename "$src" .c)
    bin="$build/$name"
    if ! "$cc" -O1 -g -fsanitize=address,undefined -D_GNU_SOURCE -DDNA_TEST \
        -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
        -o "$bin" "$src"; then
        echo "FAIL: $name (compile)" >&2
        status=1
        break
    fi
    if ! TMPDIR="$tmp" "$bin"; then
        echo "FAIL: $name" >&2
        status=1
        break
    fi
done
exit "$status"
