# This file is included by DuckDB's build system. It specifies which extension to load

# For WASM builds, we need EMULATE_FUNCTION_POINTER_CASTS=1 because cmark-gfm uses
# function pointers for iterators and renderers that cause "indirect call signature mismatch"
# errors without this flag. See: https://github.com/emscripten-core/emscripten/issues/5034
message(STATUS "extension_config.cmake: EMSCRIPTEN=${EMSCRIPTEN}, CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
if(EMSCRIPTEN)
    set(MARKDOWN_WASM_LINKED_LIBS "-sEMULATE_FUNCTION_POINTER_CASTS=1")
    message(STATUS "WASM build: markdown extension will use EMULATE_FUNCTION_POINTER_CASTS for cmark-gfm")
else()
    set(MARKDOWN_WASM_LINKED_LIBS "")
    message(STATUS "Non-WASM build: EMSCRIPTEN not defined")
endif()

# Extension from this repo
duckdb_extension_load(markdown
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    LINKED_LIBS "${MARKDOWN_WASM_LINKED_LIBS}"
)

# For WASM: Set as CACHE variable AFTER duckdb_extension_load to ensure it's visible
# in child scopes (add_subdirectory) where build_loadable_extension_directory reads it.
# Normal scope variables set by duckdb_extension_load are not visible across add_subdirectory boundaries.
if(EMSCRIPTEN)
    set(DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS "${MARKDOWN_WASM_LINKED_LIBS}" CACHE STRING "Linked libs for markdown extension" FORCE)
    message(STATUS "Set CACHE variable DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS=${DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS}")
endif()

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)