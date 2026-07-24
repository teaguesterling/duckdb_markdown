# WASM tests

Two layers guard the WASM build of the markdown extension against the
`LINKED_LIBS` class of bug (issue #19), where the loadable
`.duckdb_extension.wasm` is produced by a separate
`emcc -sSIDE_MODULE=2 ... ${LINKED_LIBS}` link. That link only pulls in
libraries named in `LINKED_LIBS` in `extension_config.cmake` — `cmark-gfm`
must be listed there or its symbols end up imported by the module but defined
nowhere, and the module fails to load ("could not load dynamic lib") or throws
on first call. CI builds the `.wasm` but never inspects it, so a broken module
otherwise ships green.

## 1. Static import check (hard gate) — `run_wasm_checks.sh`

Statically inspects the side module's imports for unresolved **cmark-gfm**
dependency symbols (`cmark_*`). No duckdb-wasm runtime and no matching duckdb
version needed, so a failure is unambiguously the `LINKED_LIBS` bug — not an
ABI/version mismatch. This is the deterministic gate.

```bash
make wasm_mvp          # or wasm_eh / wasm_threads
test/wasm/run_wasm_checks.sh
# or point it at explicit build dirs:
test/wasm/run_wasm_checks.sh build/wasm_eh
```

`check_wasm_imports.mjs` is the generic engine (shared, byte-for-byte, with the
other extensions); `run_wasm_checks.sh` supplies the markdown-specific
`--forbid cmark` pattern. See the header comment in `check_wasm_imports.mjs`
for why "is it imported?" is the wrong question and how self-resolution
(interposition) is handled.

## 2. Live load test (informational) — `load_test.cjs`

Actually `LOAD`s the built `.wasm` into a version-matched duckdb-wasm engine and
runs a cmark-gfm-backed query (`md_to_html`). End-to-end counterpart to the
static gate. Expected-red until an official duckdb-wasm ships a matching
v1.5.4 build (an engine/extension version skew surfaces as an ABI mismatch on
`LOAD`), so CI runs it `continue-on-error`.

```bash
cd test/wasm && npm install --no-save
node load_test.cjs --repo <repo-dir> --name markdown --platform eh \
  --query "SELECT md_to_html('# H') AS h" --expect "<h1>"
```
