#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# dev_build.sh - quick incremental syntax/compile check used during
# development.  The real build entry point is the top-level Makefile; this
# script simply compiles every translation unit separately so the first error
# in a file is reported immediately.
# ---------------------------------------------------------------------------
set -u

cd "$(dirname "$0")/.." || exit 1

CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -Wall -Wextra -pedantic -pthread -Iinclude"

status=0
mkdir -p build/obj

for src in "$@"; do
    obj="build/obj/$(echo "$src" | tr '/' '_').o"
    printf '== %s\n' "$src"
    if ! $CXX $CXXFLAGS -c "$src" -o "$obj"; then
        status=1
    fi
done

exit $status