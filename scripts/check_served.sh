#!/usr/bin/env bash
# Verify that the community registry SERVES this extension at an expected commit.
#
# WHY THIS EXISTS. A merged registry descriptor is not a served artifact. markdown
# v1.9.6 was tagged, its descriptor was prepared, and it never served: a platform
# leg failed in the registry build, and a failed build skips `deploy`, so the
# release registered and silently published nothing. Only an INSTALL binds.
#
# Usage:  scripts/check_served.sh <expected-commit-prefix> [duckdb-binary]
# e.g.    scripts/check_served.sh 4721b31 /path/to/stock/duckdb
#
# Exit 0 only when the registry serves THIS commit and the served binary behaves.

set -uo pipefail

EXPECT="${1:-}"
DUCKDB="${2:-duckdb}"
EXT=markdown

if [ -z "$EXPECT" ]; then
	echo "usage: $0 <expected-commit-prefix> [duckdb-binary]" >&2
	exit 2
fi
command -v "$DUCKDB" >/dev/null 2>&1 || [ -x "$DUCKDB" ] || {
	echo "FAIL: no duckdb binary at '$DUCKDB'" >&2; exit 2; }

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

q() { "$DUCKDB" -unsigned -noheader -list -c "SET extension_directory='$TMPD'; $1" 2>&1; }

fail=0
note() { printf '  %-34s %s\n' "$1" "$2"; }

echo "serve check: $EXT @ $EXPECT   (extension_directory=$TMPD)"

# --- 1. PRECONDITION, FILTERED TO THIS EXTENSION ------------------------------
# NOT a global "nothing is installed" assertion and NOT "the directory is empty".
# Both are vacuous: duckdb_extensions() reports statically linked core extensions
# as installed=true regardless of extension_directory. Measured on a stock v1.5.6
# CLI with a fresh mktemp dir: 6 installed, all install_mode=STATICALLY_LINKED,
# 0 files on disk. A check that counts globally is decoration.
# (Credit: Zim-Dev measured this; reproduced here before relying on it.)
pre=$(q "SELECT count(*) FROM duckdb_extensions() WHERE extension_name='$EXT' AND installed;")
note "precondition ($EXT installed)" "$pre"
[ "$pre" = "0" ] || { note "  => " "FAIL: expected 0 before install"; fail=1; }

# --- 2. INSTALL FROM THE REGISTRY --------------------------------------------
out=$(q "INSTALL $EXT FROM community;")
if echo "$out" | grep -qiE "error"; then
	note "INSTALL FROM community" "FAIL"
	echo "$out" | head -3 | sed 's/^/      /'
	exit 1
fi
note "INSTALL FROM community" "ok"

# --- 3. WHAT GOT INSTALLED ---------------------------------------------------
row=$(q "SELECT extension_version || '|' || install_mode || '|' || installed
         FROM duckdb_extensions() WHERE extension_name='$EXT';")
ver=$(echo "$row" | cut -d'|' -f1)
mode=$(echo "$row" | cut -d'|' -f2)
inst=$(echo "$row" | cut -d'|' -f3)
note "installed" "$inst"
note "install_mode" "$mode"
note "extension_version" "$ver"

[ "$inst" = "true" ] || { note "  => " "FAIL: not installed"; fail=1; }

# install_mode must be REPOSITORY. That single assertion rules out both
# STATICALLY_LINKED (a core extension masquerading as ours) and LOCAL_FILE (a
# developer build loaded by path), either of which would make the version
# assertion below pass for the wrong reason.
[ "$mode" = "REPOSITORY" ] || { note "  => " "FAIL: expected REPOSITORY, got '$mode'"; fail=1; }

# THE DISCRIMINATING ASSERTION. extension_version reports the COMMIT, not a
# semver string -- a served v1.9.5 reports '437e204'. Without this, "markdown is
# served" is true continuously across releases and says nothing about whether the
# release we just cut is the one being served.
case "$ver" in
	"$EXPECT"*) note "version matches expected" "yes" ;;
	*)          note "version matches expected" "FAIL: serving '$ver', expected '$EXPECT'*"; fail=1 ;;
esac

# --- 4. FUNCTIONAL PROBE ON THE SERVED BINARY --------------------------------
# Serving the right commit is necessary, not sufficient: the artifact still has
# to work. Probe the fix this release exists for -- bare FROM on a markdown path,
# which was broken in every artifact before v1.9.7 because the replacement scan
# was registered on a path the loadable extension never runs.
if [ -f test/markdown/simple.md ]; then
	got=$(q "LOAD $EXT; SELECT count(*) FROM 'test/markdown/simple.md';")
	case "$got" in
		1) note "bare FROM on served binary" "1 row" ;;
		*) note "bare FROM on served binary" "FAIL: '$got'"; fail=1 ;;
	esac
	# Control: the explicit reader does not use the replacement scan, so it works
	# even when the scan is missing. If this fails too, the problem is the load,
	# not the scan -- which is a different diagnosis.
	ctl=$(q "LOAD $EXT; SELECT count(*) FROM read_markdown('test/markdown/simple.md');")
	note "control (explicit reader)" "$ctl"
else
	note "functional probe" "skipped: run from the repo root (needs test/markdown/simple.md)"
fi

echo
if [ "$fail" = "0" ]; then
	echo "SERVED: $EXT @ $ver via $mode, and the served binary behaves."
else
	echo "NOT SERVED AS EXPECTED (see FAIL lines above)."
fi
exit "$fail"
