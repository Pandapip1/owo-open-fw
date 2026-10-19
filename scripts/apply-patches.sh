#!/usr/bin/env sh
# Apply patches/*.patch to the west-managed zephyr-lang-rust checkout and patches/zephyr/*.patch to
# the zephyr checkout (idempotent).
# Run after `west update` (which resets the module to the pinned revision).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
top=$(west topdir)
apply() { # dir patch...
  dir=$1; shift
  for p in "$@"; do
    if git -C "$dir" apply --reverse --check "$p" 2>/dev/null; then
      echo "already applied: $(basename "$p")"
    else
      git -C "$dir" apply --check "$p"
      git -C "$dir" apply "$p"
      echo "applied: $(basename "$p")"
    fi
  done
}
apply "$top/modules/lang/rust" "$here"/patches/*.patch
apply "$top/zephyr" "$here"/patches/zephyr/*.patch
