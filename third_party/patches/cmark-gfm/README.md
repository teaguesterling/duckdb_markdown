# cmark-gfm Patches for DuckDB Markdown Extension

## wasm-function-pointer-fix.patch

**Purpose:** Fix WebAssembly compatibility issue causing "indirect call signature mismatch" errors.

### The Problem

cmark-gfm contains function pointer casts that are valid in native C but fail in WebAssembly:

```c
// plugin.c:26 and registry.c:39
cmark_llist_free_full(..., (cmark_free_func) cmark_syntax_extension_free);
```

The signatures don't match:
- `cmark_free_func` expects: `(cmark_mem*, void*)`
- `cmark_syntax_extension_free` has: `(cmark_mem*, cmark_syntax_extension*)`

In native C, `void*` and `cmark_syntax_extension*` are interchangeable. In WebAssembly:
1. Emscripten places functions in **different tables** based on their C type signature
2. The call site does `call_indirect` expecting `(cmark_mem*, void*)`
3. The actual function has signature `(cmark_mem*, cmark_syntax_extension*)`
4. WASM runtime throws: `RuntimeError: indirect call signature mismatch`

### The Fix

Add wrapper functions with the correct `void*` signature:

```c
static void syntax_extension_free_wrapper(cmark_mem *mem, void *ext) {
  cmark_syntax_extension_free(mem, (cmark_syntax_extension *)ext);
}
```

### Applying the Patch

To apply manually:
```bash
cd /path/to/cmark-gfm
patch -p1 < /path/to/wasm-function-pointer-fix.patch
```

For vcpkg, create a custom port overlay that applies this patch in the portfile.

### References

- [DuckDB Markdown Issue #13](https://github.com/teaguesterling/duckdb_markdown/issues/13)
- [cmark-gfm WASM Discussion](https://github.com/github/cmark-gfm/issues/218)
