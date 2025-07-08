PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=markdown
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Override vcpkg commit to get cmark-gfm support
# This uses a newer vcpkg commit that includes cmark-gfm
export VCPKG_COMMIT := 8eb57355a4ffb410a2e94c07b4dca2dffbee8e50
export VCPKG_URL := https://github.com/Microsoft/vcpkg.git

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile