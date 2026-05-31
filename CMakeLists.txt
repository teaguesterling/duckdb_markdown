cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME markdown)

find_package(cmark-gfm CONFIG REQUIRED)
find_package(cmark-gfm-extensions CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

# WASM: Set LINKED_LIBS BEFORE build_loadable_extension is called, since that
# function reads the variable. We need to pass cmark-gfm static library paths
# to the emcc link step to actually embed the code (otherwise it stays as imports).
if(EMSCRIPTEN)
    # Get the actual library file paths from the imported targets
    get_target_property(CMARK_GFM_LIB libcmark-gfm_static IMPORTED_LOCATION)
    get_target_property(CMARK_GFM_EXT_LIB libcmark-gfm-extensions_static IMPORTED_LOCATION)

    # If IMPORTED_LOCATION is not set, try IMPORTED_LOCATION_RELEASE
    if(NOT CMARK_GFM_LIB)
        get_target_property(CMARK_GFM_LIB libcmark-gfm_static IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT CMARK_GFM_EXT_LIB)
        get_target_property(CMARK_GFM_EXT_LIB libcmark-gfm-extensions_static IMPORTED_LOCATION_RELEASE)
    endif()

    # Pass libraries to emcc via the LINKED_LIBS mechanism
    # The extension library must come before core (reverse order for linker)
    set(DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS "${CMARK_GFM_EXT_LIB} ${CMARK_GFM_LIB}" CACHE STRING "" FORCE)
    message(STATUS "WASM: cmark-gfm libraries for linking: ${DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS}")
endif()

set(EXTENSION_SOURCES
    src/markdown_extension.cpp
    src/markdown_reader_functions.cpp
    src/markdown_reader_files.cpp
    src/markdown_copy.cpp
    src/markdown_types.cpp
    src/markdown_scalar_functions.cpp
    src/markdown_extraction_functions.cpp
    src/markdown_utils.cpp
    src/duck_block_functions.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
# Windows MSVC needs /std:c++17 to compile duckdb's vendored fmt/format.h
# (which uses C++17 inline variables). MSVC defaults to /std:c++14. duckdb's
# own Windows build must already use C++17 since it vendors fmt and links it
# into duckdb_static; aligning our extension targets here keeps the standard
# consistent across the link. Not applied on Linux: forcing C++17 on our
# extension there causes static-const data members in duckdb's headers to
# acquire implicit inline linkage in our TUs but not in libduckdb's
# C++11-built TUs, producing multiple-definition link errors.
if(MSVC)
    target_compile_options(${EXTENSION_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++17>")
    target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/std:c++17>")
endif()


# Link cmark-gfm in both the static library and the loadable extension
# Note: Order matters - extensions depends on core, so core must come after extensions
target_link_libraries(${EXTENSION_NAME} libcmark-gfm-extensions_static libcmark-gfm_static)
target_link_libraries(${LOADABLE_EXTENSION_NAME} libcmark-gfm-extensions_static libcmark-gfm_static)

# DuckDB extension requires C++17
set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD 17)

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
