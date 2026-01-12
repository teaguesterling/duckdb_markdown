cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME markdown)

find_package(cmark-gfm CONFIG REQUIRED)
find_package(cmark-gfm-extensions CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

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

# For WASM builds, add Emscripten flag to handle function pointer signature mismatches in cmark-gfm
# See: https://github.com/emscripten-core/emscripten/issues/5034
# cmark-gfm uses function pointers for iterators and renderers that cause "indirect call signature mismatch"
# errors in WASM without this flag.
if(EMSCRIPTEN)
  set(DUCKDB_EXTENSION_MARKDOWN_LINKED_LIBS "-sEMULATE_FUNCTION_POINTER_CASTS=1" CACHE STRING "" FORCE)
  message(STATUS "WASM build: enabling EMULATE_FUNCTION_POINTER_CASTS for cmark-gfm compatibility")
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link cmark-gfm in both the static library and the loadable extension
# Note: Order matters - extensions depends on core, so core must come after extensions
target_link_libraries(${EXTENSION_NAME} libcmark-gfm-extensions_static libcmark-gfm_static)
target_link_libraries(${LOADABLE_EXTENSION_NAME} libcmark-gfm-extensions_static libcmark-gfm_static)

# For WASM builds, also add the flag directly to the target link options as a fallback
if(EMSCRIPTEN)
  target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE "-sEMULATE_FUNCTION_POINTER_CASTS=1")
  message(STATUS "WASM build: added EMULATE_FUNCTION_POINTER_CASTS to ${LOADABLE_EXTENSION_NAME} link options")
endif()

# DuckDB extension requires C++17
set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD 17)

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
