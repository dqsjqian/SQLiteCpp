#!/usr/bin/env bash
# Picks the newest GCC and/or LLVM preinstalled on the runner image and
# exports them to later steps. ubuntu-latest moves from 24.04 to 26.04 between
# 2026-10-19 and 2026-11-19, so CI never hardcodes a compiler version.
#
# Usage: select-toolchain.sh [gcc] [llvm] [default=gcc|default=llvm]
# Exports: GCC_CC GCC_CXX GCOV_TOOL               (gcc)
#          LLVM_VER CLANG_CC CLANG_CXX CLANG_TIDY  (llvm)
#          CC CXX                                  (default=..., also selects it)
# Variables written to $GITHUB_ENV are visible from the next step on.
set -euo pipefail

emit() { echo "$1=$2" >> "$GITHUB_ENV"; echo "  $1=$2"; }

. /etc/os-release
echo "image: $PRETTY_NAME ($(uname -m))"

default=""
want_gcc=0
want_llvm=0
for arg in "$@"; do
  case "$arg" in
    default=gcc)  default=gcc;  want_gcc=1 ;;
    default=llvm) default=llvm; want_llvm=1 ;;
    gcc)  want_gcc=1 ;;
    llvm) want_llvm=1 ;;
    *) echo "::error::unknown argument '$arg'"; exit 1 ;;
  esac
done
wants=()
[ "$want_gcc" = 1 ] && wants+=(gcc)
[ "$want_llvm" = 1 ] && wants+=(llvm)
[ "${#wants[@]}" -gt 0 ] || { echo "::error::nothing to select"; exit 1; }

for want in "${wants[@]}"; do
  case "$want" in
    gcc)
      cc=gcc; cxx=g++; gcov=gcov
      for v in 17 16 15 14 13; do
        if command -v "g++-$v" >/dev/null 2>&1; then
          cc="gcc-$v"; cxx="g++-$v"; gcov="gcov-$v"; break
        fi
      done
      command -v "$cxx" >/dev/null 2>&1 || { echo "::error::no g++ on this image"; exit 1; }
      emit GCC_CC "$cc"; emit GCC_CXX "$cxx"; emit GCOV_TOOL "$gcov"
      if [ "$default" = gcc ]; then emit CC "$cc"; emit CXX "$cxx"; fi
      "$cxx" --version | head -1
      ;;
    llvm)
      ver=""
      for v in 25 24 23 22 21 20 19 18; do
        if command -v "clang++-$v" >/dev/null 2>&1; then ver=$v; break; fi
      done
      [ -n "$ver" ] || { echo "::error::no versioned clang++ on this image"; exit 1; }
      emit LLVM_VER "$ver"; emit CLANG_CC "clang-$ver"; emit CLANG_CXX "clang++-$ver"
      emit CLANG_TIDY "clang-tidy-$ver"
      if [ "$default" = llvm ]; then emit CC "clang-$ver"; emit CXX "clang++-$ver"; fi
      "clang++-$ver" --version | head -1
      ;;
  esac
done
