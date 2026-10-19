#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TODO_FILE=${TODO_FILE:-"$ROOT/todo.txt"}

cp "$TODO_FILE" "$1"
