#!/usr/bin/env bash
# Compiles every public header on its own: each one must be self-contained.
# Usage: check-headers.sh <c++ compiler> [extra flags...]
set -euo pipefail
compiler=$1; shift
status=0
probe_dir=$(mktemp -d)
probe="$probe_dir/header_check.cc"
for header in include/SQLiteCpp.h include/SQLiteCpp/*.h include/SQLiteCpp/detail/*.h; do
  case "$header" in
    include/SQLiteCpp/sqlcipher.h) continue ;;  # needs the SQLCipher extension
  esac
  printf '#include <%s>\nint main() { return 0; }\n' "${header#include/}" > "$probe"
  if ! "$compiler" -std=c++23 -Iinclude -Ivendor/sqlite -Wall -Wextra -Wpedantic -Werror \
       "$@" -fsyntax-only "$probe"; then
    echo "::error file=$header::not self-contained under $compiler"
    status=1
  fi
done
rm -rf "$probe_dir"
if [ "$status" -eq 0 ]; then
  echo "every public header is self-contained under $("$compiler" --version | head -1)"
fi
exit "$status"
