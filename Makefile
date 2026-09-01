PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=markdown
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Check the vendored duck_block vocabulary against upstream duck_block_utils.
# A vendored copy does not notice when upstream moves, and a changed VALUE
# (rather than a renamed constant) is invisible to the compiler regardless.
# Pass --upstream PATH to compare against a local clone instead of the network.
.PHONY: check-vocabulary
check-vocabulary:
	python3 scripts/check_duck_block_vocabulary.py
