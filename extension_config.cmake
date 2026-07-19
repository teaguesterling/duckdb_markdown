# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(markdown
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    # WASM: the SIDE_MODULE=2 link only embeds libs named here (it ignores
    # target_link_libraries / INTERFACE_LINK_LIBRARIES), so cmark-gfm must be listed
    # explicitly or the .wasm loads with stub imports that throw on first call (issue #19).
    # Order mirrors the native link (extensions depend on core, so core comes last).
    LINKED_LIBS "$<TARGET_FILE:libcmark-gfm-extensions_static>;$<TARGET_FILE:libcmark-gfm_static>"
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
