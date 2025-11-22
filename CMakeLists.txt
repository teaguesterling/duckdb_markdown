cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME markdown)

find_package(cmark-gfm CONFIG REQUIRED)
find_package(cmark-gfm-extensions CONFIG REQUIRED)
find_package(unofficial-gumbo CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/markdown_extension.cpp
    src/markdown_reader_functions.cpp
    src/markdown_reader_files.cpp
    src/markdown_types.cpp
    src/markdown_scalar_functions.cpp
    src/markdown_extraction_functions.cpp
    src/markdown_utils.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link cmark-gfm and gumbo in both the static library and the loadable extension
# Note: Order matters - extensions depends on core, so core must come after extensions
target_link_libraries(${EXTENSION_NAME} libcmark-gfm-extensions_static libcmark-gfm_static unofficial::gumbo::gumbo)
target_link_libraries(${LOADABLE_EXTENSION_NAME} libcmark-gfm-extensions_static libcmark-gfm_static unofficial::gumbo::gumbo)

# DuckDB extension requires C++17
set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD 17)

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
