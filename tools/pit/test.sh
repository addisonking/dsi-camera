#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cc -std=c11 -Wall -Wextra -I"$root/tools/pit/shim" -I"$root/arm9/include" \
  -DPIT_PATH='"pit.bin"' "$root/arm9/source/pit.c" "$root/tools/pit/test.c" -o "$tmp/test"
cd "$tmp"
./test
