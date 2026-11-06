#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
DIR=$(mktemp -d "${TMPDIR:-/tmp}/nuvio-subtap.XXXXXX")
trap 'rm -rf "$DIR"' EXIT
cc -Isrc -O1 -g -Wall -Wextra -Werror -pthread tests/subtap.c src/subtap.c src/pgs.c -o "$DIR/test"
"$DIR/test"
