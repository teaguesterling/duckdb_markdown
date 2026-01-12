# This file is included by DuckDB's build system. It specifies which extension to load

# For WASM builds, we need EMULATE_FUNCTION_POINTER_CASTS=1 because cmark-gfm uses
# function pointers for iterators and renderers that cause "indirect call signature mismatch"
# errors without this flag. See: https://github.com/emscripten-core/emscripten/issues/5034
if(EMSCRIPTEN)
    set(MARKDOWN_WASM_LINKED_LIBS "-sEMULATE_FUNCTION_POINTER_CASTS=1")
    message(STATUS "WASM build: markdown extension will use EMULATE_FUNCTION_POINTER_CASTS for cmark-gfm")
else()
    set(MARKDOWN_WASM_LINKED_LIBS "")
endif()

# Extension from this repo
duckdb_extension_load(markdown
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    LINKED_LIBS "${MARKDOWN_WASM_LINKED_LIBS}"
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)