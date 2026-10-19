#!/usr/bin/env sh
# Apply patches/ to the west-managed zephyr-lang-rust checkout (idempotent).
# Run after `west update` (which resets the module to the pinned revision).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
mod="$(west topdir)/modules/lang/rust"
for p in "$here"/patches/*.patch; do
  if git -C "$mod" apply --reverse --check "$p" 2>/dev/null; then
    echo "already applied: $(basename "$p")"
  else
    git -C "$mod" apply --check "$p"
    git -C "$mod" apply "$p"
    echo "applied: $(basename "$p")"
  fi
done
