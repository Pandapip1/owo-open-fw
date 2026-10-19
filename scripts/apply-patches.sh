#!/usr/bin/env sh
# Apply patches/ to the west-managed checkouts (idempotent). Run after `west update`.
#   patches/*.patch        -> modules/lang/rust (zephyr-lang-rust)
#   patches/zephyr/*.patch -> zephyr itself (Counter-backed timing backend, 74HC595 read-back)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
top=$(west topdir)
apply() { # dir patch
  if git -C "$1" apply --reverse --check "$2" 2>/dev/null; then
    echo "already applied: $(basename "$2")"
  else
    git -C "$1" apply --check "$2"
    git -C "$1" apply "$2"
    echo "applied: $(basename "$2")"
  fi
}
for p in "$here"/patches/*.patch; do apply "$top/modules/lang/rust" "$p"; done
for p in "$here"/patches/zephyr/*.patch; do apply "$top/zephyr" "$p"; done
