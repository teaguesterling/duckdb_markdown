# WASM Support for DuckDB Markdown Extension

This document describes the WebAssembly (WASM) support for the DuckDB Markdown extension, including architecture details, known challenges, and the solutions implemented.

## Overview

The markdown extension is fully compatible with DuckDB-WASM, allowing all markdown functions to run in web browsers:

- `read_markdown()` - Read markdown files as HTML
- `read_markdown_sections()` - Extract document sections
- `read_markdown_blocks()` - Parse markdown into individual block elements

## Architecture

### How DuckDB WASM Extensions Work

DuckDB-WASM uses Emscripten's dynamic linking to load extensions at runtime:

1. **Main Module**: The core DuckDB-WASM binary (`duckdb-eh.wasm`) is compiled as an Emscripten main module
2. **Side Modules**: Extensions are compiled as Emscripten side modules (`-sSIDE_MODULE=2`)
3. **Dynamic Linking**: When an extension is loaded, DuckDB-WASM uses `dlopen`/`dlsym` to link the side module

Side modules can import symbols from the main module (DuckDB APIs, standard library functions) and must export their initialization function.

### The cmark-gfm Dependency

This extension uses [cmark-gfm](https://github.com/github/cmark-gfm) (GitHub Flavored Markdown parser) as a third-party dependency. Making this work in WASM required solving two key challenges.

## Challenge 1: Static Library Linking

### The Problem

By default, when building a WASM side module, Emscripten leaves external symbols as unresolved imports. This means cmark-gfm functions would be imported from the main module:

```
Import: func[207] sig=1 <env.cmark_parser_new> <- env.cmark_parser_new
```

But the main DuckDB-WASM module doesn't contain cmark-gfm, so these imports fail at runtime with errors like:
- `"r is not a function"` (during dlopen)
- `"table index is out of bounds"` (if partially resolved)

### The Solution

The cmark-gfm static libraries must be explicitly linked into the WASM side module during the final emcc linking step. This is done via DuckDB's `LINKED_LIBS` mechanism:

```cmake
# CMakeLists.txt
if(EMSCRIPTEN)
    # Get the actual library file paths from the imported targets
    get_target_property(CMARK_GFM_LIB libcmark-gfm_static IMPORTED_LOCATION)
    get_target_property(CMARK_GFM_EXT_LIB libcmark-gfm-extensions_static IMPORTED_LOCATION)

    # Fallback to RELEASE configuration if needed
    if(NOT CMARK_GFM_LIB)
        get_target_property(CMARK_GFM_LIB libcmark-gfm_static IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT CMARK_GFM_EXT_LIB)
        get_target_property(CMARK_GFM_EXT_LIB libcmark-gfm-extensions_static IMPORTED_LOCATION_RELEASE)
    endif()

    # Pass libraries to emcc via the LINKED_LIBS mechanism
    # Extension library must come before core (reverse order for linker)
    set(DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS
        "${CMARK_GFM_EXT_LIB} ${CMARK_GFM_LIB}"
        CACHE STRING "" FORCE)
endif()
```

**Critical**: This block MUST appear BEFORE `build_loadable_extension()` is called, because that function reads the `LINKED_LIBS` variable.

### How It Works

DuckDB's build system (in `duckdb/CMakeLists.txt`) uses a post-build command:

```cmake
add_custom_command(
  TARGET ${TARGET_NAME}
  POST_BUILD
  COMMAND emcc $<TARGET_FILE:${TARGET_NAME}> -o $<TARGET_FILE:${TARGET_NAME}>.wasm
          -O3 -sSIDE_MODULE=2
          -sEXPORTED_FUNCTIONS="${EXPORTED_FUNCTIONS}"
          ${WASM_THREAD_FLAGS}
          ${TO_BE_LINKED}  # <-- Our libraries are passed here
)
```

### Verification

You can verify the libraries are correctly embedded using `wasm-objdump`:

```bash
# Check imports - should NOT see env.cmark_* functions
wasm-objdump -j Import -x markdown.duckdb_extension.wasm | grep "env\.cmark"

# Check exports - should see cmark functions exported
wasm-objdump -j Export -x markdown.duckdb_extension.wasm | grep cmark
```

## Challenge 2: Function Pointer Signature Mismatches

### The Problem

WASM uses strict type checking for indirect function calls (`call_indirect`). When C code casts function pointers between incompatible signatures, it works on native platforms but fails in WASM:

```c
// In cmark-gfm's plugin.c - this pattern breaks WASM:
cmark_llist_free_full(&CMARK_DEFAULT_MEM_ALLOCATOR,
                      plugin->syntax_extensions,
                      (cmark_free_func) cmark_syntax_extension_free);  // Cast!
```

The issue:
- `cmark_free_func` expects: `void (*)(cmark_mem*, void*)`
- `cmark_syntax_extension_free` has: `void (*)(cmark_mem*, cmark_syntax_extension*)`

In WASM, these are different function table entries, causing:
```
RuntimeError: indirect call signature mismatch
```
or
```
RuntimeError: table index is out of bounds
```

### The Solution

We maintain a patch for cmark-gfm that adds wrapper functions with correct signatures:

```c
// Wrapper function to avoid function pointer cast that breaks WASM
static void syntax_extension_free_wrapper(cmark_mem *mem, void *ext) {
    cmark_syntax_extension_free(mem, (cmark_syntax_extension *)ext);
}

// Use the wrapper instead of casting
cmark_llist_free_full(&CMARK_DEFAULT_MEM_ALLOCATOR,
                      plugin->syntax_extensions,
                      syntax_extension_free_wrapper);  // No cast needed
```

The patch is located at: `third_party/vcpkg_ports/cmark-gfm/wasm-function-pointer-fix.patch`

### Why Not EMULATE_FUNCTION_POINTER_CASTS?

Emscripten provides `-sEMULATE_FUNCTION_POINTER_CASTS` which could theoretically fix this, but:

1. It must be enabled on BOTH the main module AND side module
2. We don't control the main DuckDB-WASM build
3. Even if enabled on both, it breaks `dlopen`/`dlsym` (Emscripten issue #13076)

The wrapper function approach is the correct solution for side modules.

## Build Configuration

### vcpkg Port Overlay

The extension uses a custom vcpkg port overlay for cmark-gfm:

```
third_party/vcpkg_ports/
└── cmark-gfm/
    ├── portfile.cmake
    ├── vcpkg.json
    └── wasm-function-pointer-fix.patch
```

This ensures the WASM-compatible patch is applied when building cmark-gfm for the `wasm32-emscripten` triplet.

### vcpkg.json

```json
{
  "dependencies": ["cmark-gfm"],
  "overrides": [
    { "name": "cmark-gfm", "version": "0.29.0.13" }
  ],
  "vcpkg-configuration": {
    "overlay-ports": ["./third_party/vcpkg_ports"]
  }
}
```

## Testing

### CI/CD Pipeline

The WASM build and test process:

1. **Build Phase** (`MainDistributionPipeline.yml`):
   - Builds extension for multiple platforms including `wasm32-emscripten`
   - Produces artifacts: `markdown-v1.4.3-extension-wasm_eh`, etc.

2. **Test Phase** (`WasmTest.yml`):
   - Downloads the built WASM extension
   - Runs browser-based tests using Playwright + Chromium
   - Tests all three markdown functions

### Version Compatibility

**Critical**: The extension version must match the duckdb-wasm runtime version:

| duckdb-wasm Version | Extension Artifact |
|---------------------|-------------------|
| 1.32.0 (stable)     | `markdown-v1.4.3-extension-wasm_eh` |
| Latest (main)       | `markdown-main-extension-wasm_eh` |

Using mismatched versions causes ABI incompatibility errors like `"r is not a function"`.

### Local Testing

To test WASM locally:

1. Build the extension:
   ```bash
   make wasm_eh
   ```

2. Serve the test page:
   ```bash
   # Use the wasm-test.html file in the repository root
   python -m http.server 8080
   ```

3. Open browser to `http://localhost:8080/wasm-test.html`

### Debugging WASM Issues

1. **Check imports/exports**:
   ```bash
   wasm-objdump -h markdown.duckdb_extension.wasm  # Section overview
   wasm-objdump -j Import -x markdown.duckdb_extension.wasm | grep env  # Check imports
   ```

2. **Common error patterns**:
   - `"r is not a function"` → Version mismatch or unresolved imports
   - `"table index is out of bounds"` → Function pointer signature mismatch
   - `"indirect call signature mismatch"` → Same as above, different WASM runtime

3. **Verify static linking worked**:
   ```bash
   # Should see cmark functions as exports, NOT as env.* imports
   wasm-objdump -j Export -x markdown.duckdb_extension.wasm | grep cmark_parser
   ```

## Limitations

### File System Access

WASM runs in a sandboxed environment without direct file system access. Files must be:
- Registered via `db.registerFileText()` or `db.registerFileBuffer()`
- Fetched from URLs that support CORS

### Performance

WASM execution is generally slower than native code. For large markdown documents, consider:
- Processing on the server when possible
- Using streaming/chunked processing for very large files

## References

- [DuckDB WASM Documentation](https://duckdb.org/docs/api/wasm/overview)
- [Emscripten Dynamic Linking](https://emscripten.org/docs/compiling/Dynamic-Linking.html)
- [WebAssembly Dynamic Linking Convention](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md)
- [cmark-gfm GitHub Repository](https://github.com/github/cmark-gfm)
- [GitHub Issue #13 - WASM Compatibility](https://github.com/teaguesterling/duckdb_markdown/issues/13)
