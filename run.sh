#!/bin/sh
set -eu
SOLUTION_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
make -s -C "$SOLUTION_DIR" pager
exec "$SOLUTION_DIR/pager" "$@"
