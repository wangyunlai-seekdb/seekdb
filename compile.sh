#!/usr/bin/env bash
set -euo pipefail
out=$1
compiler=$2
shift 2
"$compiler" -std=gnu++17 -O2 -fno-omit-frame-pointer -I. -Igenerated "$@" \
  generated/bounds.cpp generated/jump.cpp generated/allocator.cpp probe.cpp touch.cpp -o "$out"
