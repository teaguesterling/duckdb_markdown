#!/usr/bin/env bash
# Run the WASM dependency-symbol check against built .wasm artifacts.
#
# This is the fast, deterministic gate for the LINKED_LIBS class of bug
# (issue #19; see check_wasm_imports.mjs for the full explanation). It does NOT
# need a duckdb-wasm runtime or a matching duckdb version — it statically
# inspects the side module's imports. Run it after `make wasm_mvp` / `wasm_eh`.
#
# Usage: test/wasm/run_wasm_checks.sh [build-dir ...]
#   defaults to build/wasm_mvp build/wasm_eh build/wasm_threads if present.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"

# Dependency symbols that MUST be resolved internally (not left as imports).
# markdown's native dependency is cmark-gfm (C library); all of its symbols are
# prefixed `cmark_` (e.g. cmark_parser_new, cmark_markdown_to_html,
# cmark_gfm_core_extensions_ensure_registered). If cmark-gfm is missing from
# LINKED_LIBS these become unresolved imports and the .wasm fails to load.
FORBID=( --forbid 'cmark' )

dirs=("$@")
if [ ${#dirs[@]} -eq 0 ]; then
  for d in build/wasm_mvp build/wasm_eh build/wasm_threads; do
    [ -d "$repo_root/$d" ] && dirs+=("$repo_root/$d")
  done
fi

if [ ${#dirs[@]} -eq 0 ]; then
  echo "no wasm build dirs found; build first (e.g. make wasm_mvp)" >&2
  exit 2
fi

found_any=0
rc=0
for d in "${dirs[@]}"; do
  while IFS= read -r -d '' wasm; do
    found_any=1
    echo "==> checking $wasm"
    node "$here/check_wasm_imports.mjs" "$wasm" "${FORBID[@]}" || rc=1
  done < <(find "$d" -name '*.duckdb_extension.wasm' -print0 2>/dev/null)
done

if [ "$found_any" -eq 0 ]; then
  echo "no *.duckdb_extension.wasm found under: ${dirs[*]}" >&2
  exit 2
fi
exit $rc
