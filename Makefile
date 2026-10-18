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

# Render live duck_block_utils output through this writer. Complements the test
# suite rather than duplicating it: the suite replays producer output as literals
# so it can never skip, but literals only contain shapes someone already knew
# about. Skips loudly when the producer is unavailable.
.PHONY: check-producer
check-producer:
	python3 scripts/check_against_producer.py

# Check this reader's output against the canonical duck_block conformance rules.
# Unlike check-producer it has no NETWORK dependency and cannot skip. It does
# need a BUILD: the rules are pure SQL, but the reader under test is this
# extension and the script LOADs it. Saying "pure SQL, no extension" is what put
# this in a CI job with no build, where it failed five runs straight.
# duck_blocks_validate() lives in an extension this repo cannot load at all, so
# before these macros existed nothing had ever checked this reader's output
# against the canonical rules.
.PHONY: check-conformance
check-conformance:
	python3 scripts/check_conformance.py

# HOW TO VERIFY A BRANCH BY HAND, since several registries in these scripts carry
# a "WATCHED" note saying to re-plant an entry and watch it fire, and none of them
# said how to look. The rule, learned the expensive way on 2026-09-01:
#
#   run the thing under test UNPIPED, ONE input at a time, and print the WHOLE
#   output before drawing anything from it.
#
# Three false readings in one investigation that night, none from the code: a
# glob that expanded to two paths so the command under test got a nonsense
# argument; `| tail -4` truncating the error above the output being read; and
# `rc=$?` after a pipe reporting tail's exit code rather than the program's. All
# three die under that rule.
#
# The uncomfortable half, worth keeping because it is not obvious: what caught it
# was the SIZE of the conclusion, not the method. It would have retracted a
# premise this repo's tooling is built on, which is loud enough to force a second
# look. A smaller false reading goes straight into the commit message. So
# conclusion-size is doing the work verification should do, and the SMALL
# findings are the unguarded ones. (duck_block_utils' framing, and it is the
# durable part of that exchange.)
#
# An automated check gets perturbed at least once. A shell pipeline gets read and
# believed every time.

# Run EVERY check and report every failure.
#
# Each check is a separate target, which meant the only way to run them all was
# to remember all three -- and running two of three is indistinguishable from
# running three when you only read the last line. Recipe lines also stop at the
# first failure, so a naive `check: check-vocabulary check-conformance
# check-producer` would leave later checks unrun and look like a single problem
# when there might be three. (duck_block_utils hit exactly this: eight scripts as
# sequential recipe lines, a failure in check 2 left 6 of 8 unrun.)
#
# So: keep going, collect names, and print what FAILED and what never ran.
.PHONY: check
check:
	@fail=""; \
	for c in "vocabulary:scripts/check_duck_block_vocabulary.py" \
	         "conformance:scripts/check_conformance.py" \
	         "producer:scripts/check_against_producer.py"; do \
	  name=$${c%%:*}; script=$${c##*:}; \
	  printf '\n=== %s ===\n' "$$name"; \
	  python3 "$$script" || fail="$$fail $$name"; \
	done; \
	printf '\n'; \
	if [ -n "$$fail" ]; then echo "FAILED:$$fail"; exit 1; else echo "all checks passed"; fi

.PHONY: test-require
# A TEST RUN THAT SKIPPED REPORTS SUCCESS. `require markdown` in a .test file means
# "skip unless available", so when the extension cannot be loaded EVERY file skips and
# the harness still prints "all tests passed" with exit 0. That is how 680 tests across
# this fleet went unrun behind 12 green registry checks in October 2026
# (duckdb/community-extensions#3086: the recorded extension version is truncated to 32
# bytes while the engine compares the untruncated value, so `require` finds nothing).
#
# `--require <name>` makes a MISSING require a hard failure instead -- see
# sqllogic_test_runner.cpp, where IsRequired(param) turns it into
# parser.Fail("require %s: FAILED"). It is SCOPED to the named requirement, so files
# gated on anything else (json, notwindows) still skip normally. That scoping is why this
# is used rather than run_tests.py's --fail-require-skip, which collapses every skip
# reason into one string and so cannot tell an unloadable extension from a deliberate
# platform gate -- and which is a no-op as shipped anyway, searching stdout while the
# summary goes to stderr (duckdb/duckdb#27413, with a verified fix).
#
# THE NAME IS WRITTEN OUT, not $(EXT_NAME), because the two are not always the same:
# sitting_duck sets EXT_NAME=duckdb_ast while its tests say `require sitting_duck`, so
# $(EXT_NAME) there would match no token and make this guard a silent no-op -- the exact
# failure class it exists to catch.
#
# WATCHED IT FAIL BOTH WAYS on DuckDB 2.0 (b15358c371d) before being trusted, by moving
# the built extension aside (measured on duck_block_utils):
#     make test-require, present : exit 0  "2 skipped, 813 assertions in 16 cases"
#     make test-require, hidden  : exit 2  "require duck_block_utils: FAILED"
#     make test      , hidden    : exit 0  "ran tests: 0 passed, 18 skipped"  <-- the bug
# The present case's legitimate skips passing is the proof the scoping works.
#
# INERT ON THE v1.5.6 LINE, where `require` cannot fail: hiding the artifact changes
# nothing (measured on markdown and yaml). Harmless there, load-bearing on 2.0.
# Report the ASSERTION count this prints, never `make test`'s file count.
test-require:
	./build/release/test/unittest --require markdown "$(TESTS_BASE_DIRECTORY)*"
