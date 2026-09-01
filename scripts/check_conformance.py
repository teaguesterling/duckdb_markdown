#!/usr/bin/env python3
"""Check this reader's output against the canonical duck_block conformance rules.

WHY THIS IS A SCRIPT AND NOT A TEST. The rules live in
conformance/duck_block_conformance.sql, loaded with DuckDB's `.read`, which is a
CLI dot-command and not SQL -- sqllogictest cannot use it. Inlining the macros
into a .test would make a THIRD copy of the vocabulary, which is the defect this
whole arrangement exists to avoid.

WHY IT CANNOT SKIP. duck_blocks_validate() lives in duck_block_utils, which this
repo cannot load at all: DuckDB matches extension ABI on the exact version string
and this build reports a dev hash from a pinned submodule. So this reader's
output had never been checked against the canonical rules by anything, ever.
These macros are pure SQL and need nothing but DuckDB, so unlike
check-producer this has no dependency that can go missing and no skip path.

CONTROL FIRST. Every silence here is worthless unless the macros can object --
their type check accepted ANY string until it was fixed upstream on 2026-09-01.
"""
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Overridable so CI can run this against the BUILT ARTIFACT with a released
# duckdb CLI. Locally both default to this repo's build. In CI there is no local
# build in the lightweight job -- which is how this check spent its first four
# runs failing with "missing this extension's build", correctly refusing to skip
# in a job that could never satisfy it.
EXT = os.environ.get("MARKDOWN_EXTENSION") or os.path.join(
    REPO, "build/release/extension/markdown/markdown.duckdb_extension")
DUCKDB = os.environ.get("DUCKDB_BIN") or os.path.join(REPO, "build/release/duckdb")
RULES = os.path.join(REPO, "conformance/duck_block_conformance.sql")

# (name, sql expression, expected literal). Controls first, deliberately.
CASES = [
    ("CONTROL undeclared type is reported",
     "duck_blocks_undeclared_types([{kind:'block',element_type:'zzzz_not_a_type',content:'x',"
     "level:1,encoding:'text',attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:0}])",
     "[zzzz_not_a_type]"),
    ("CONTROL invalid element is rejected",
     "duck_block_is_valid({kind:'block',element_type:'paragraph',content:'x',level:0,"
     "encoding:'text',attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:0})",
     "false"),
    ("prose is conformant",
     r"duck_blocks_are_valid(parse_markdown_to_duck_blocks(e'# H\n\nBody **b** and [l](http://x).\n'))",
     "true"),
    ("lists, quotes, tables, code are conformant",
     r"bool_and(duck_blocks_are_valid(parse_markdown_to_duck_blocks(md))) FROM (VALUES "
     r"(e'- one\n- two\n'),(e'3. one\n4. two\n'),(e'> q\n'),(e'| A |\n|---|\n| 1 |\n'),"
     r"(e'```py\nx=1\n```\n'),(e'Plain.\n')) d(md)",
     "true"),
    ("every emitted type is declared",
     r"duck_blocks_undeclared_types(parse_markdown_to_duck_blocks(e'# H\n\nB **b**.\n\n- i\n\n> q\n'))",
     "[]"),
    # Was "[frontmatter]" until spec 6.2 settled the name as `metadata` +
    # attributes['role']='frontmatter'. Kept as an assertion rather than deleted:
    # it is the one document that USED to carry an undeclared type, so it is the
    # one most worth holding at zero.
    ("a frontmatter document emits no undeclared type",
     r"duck_blocks_undeclared_types(parse_markdown_to_duck_blocks(e'---\ntitle: T\n---\n\nB.\n'))",
     "[]"),
    # The role is what now carries what the old type name said. Asserted so that
    # renaming the type back, or dropping the role, is a failure and not a silence.
    ("the metadata block carries role='frontmatter'",
     r"""(SELECT list(e.attributes['role']) FROM (SELECT unnest(parse_markdown_to_duck_blocks(e'---\ntitle: T\n---\n\nB.\n')) AS e) WHERE e.element_type = 'metadata')""",
     "[frontmatter]"),
    # Was the one recorded exception: frontmatter emitted level 0, which
    # duck_block_is_valid rejects. Ruled level 1 upstream at 2771e9e and fixed, so
    # this asserts CONFORMANCE now. Kept rather than deleted -- it is the document
    # that was non-conformant, so it is the one worth holding conformant.
    ("a frontmatter document is fully conformant",
     r"duck_blocks_are_valid(parse_markdown_to_duck_blocks(e'---\ntitle: T\n---\n\nB.\n'))",
     "true"),
    # The level itself, asserted directly so a regression to 0 names the field
    # rather than showing up as a bare "invalid".
    ("the metadata block carries level 1, not 0",
     r"""(SELECT list(e.level) FROM (SELECT unnest(parse_markdown_to_duck_blocks(e'---\ntitle: T\n---\n\nB.\n')) AS e) WHERE e.element_type = 'metadata')""",
     "[1]"),
    # TOML frontmatter takes the same path and must be conformant too -- it is a
    # newer branch of the same scanner, so it is the likelier one to regress.
    #
    # Asserts the ENCODING rather than mere validity, because validity passes
    # VACUOUSLY on a build that does not recognise `+++`: the fence falls through
    # to prose, and prose is conformant. Caught by running this against an older
    # artifact, where it reported ok for a reader that had no TOML support at all.
    ("a TOML frontmatter document yields a toml metadata block",
     r"""(SELECT list(e.encoding) FROM (SELECT unnest(parse_markdown_to_duck_blocks(e'+++\ntitle = "T"\n+++\n\nB.\n')) AS e) WHERE e.element_type = 'metadata')""",
     "[toml]"),
    ("...and that document is conformant",
     r"duck_blocks_are_valid(parse_markdown_to_duck_blocks(e'+++\ntitle = \"T\"\n+++\n\nB.\n'))",
     "true"),
]



def absorption_cases():
    """Every declared type, checked for the run-eating bug.

    A block ABSORBS the inline run that follows it. Three declared types render
    without using their content -- `hr`, `page_break`, `generic` -- so absorbing
    into one DELETED the run: an `hr` followed by "Section two" emitted the rule
    and dropped the sentence, in duck_blocks_to_md itself. It survived because
    the only path that exercised it rendered one element at a time and never
    absorbed anything.

    Generated from the VENDORED type list rather than a list kept here, so a new
    content-discarding type upstream fails this the first time it is vendored,
    instead of quietly eating runs until someone reads the output.
    """
    m = re.search(r"duck_block_declared_types\(\) AS \(\s*\[(.*?)\]",
                  open(RULES).read(), re.S)
    types = re.findall(r"'([^']+)'", m.group(1)) if m else []
    if not types:
        return []
    values = ",".join(f"('{t}')" for t in types)
    probe = (lambda marker:
             "(SELECT coalesce(list(t), []) FROM (VALUES " + values + ") v(t) WHERE "
             "duck_blocks_to_md(["
             "{kind:'block',element_type:v.t,content:'',level:1,encoding:'text',"
             "attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:0},"
             "{kind:'inline',element_type:'text',content:'KEEPME',level:2,encoding:'text',"
             "attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:1}"
             "]) NOT LIKE '%" + marker + "%')")
    # CONTROL: with a marker that is never emitted, EVERY type must be reported.
    # Without it a probe that silently matched nothing would read as "no type
    # loses the run" -- a pass that means the detection is broken, not that the
    # renderer is right.
    control = ("(SELECT count(*) FROM (VALUES " + values + ") v(t) WHERE "
               "duck_blocks_to_md([{kind:'block',element_type:v.t,content:'',level:1,"
               "encoding:'text',attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:0}]) "
               "NOT LIKE '%ZZ_NEVER_EMITTED%')")
    return [
        (f"CONTROL the run probe can detect loss (all {len(types)} types)", control, str(len(types))),
        ("no declared type eats the inline run that follows it", probe("KEEPME"), "[]"),
    ]


def main():
    for what, ok in (("this extension's build", os.path.exists(EXT)),
                     ("the vendored rules at conformance/", os.path.exists(RULES))):
        if not ok:
            print(f"FAILED: missing {what}. This check has no skip path by design.")
            return 1

    cases = CASES + absorption_cases()

    sql = [f"LOAD '{EXT}';", f".read {RULES}", ".mode list", ".header off"]
    sql += [f"SELECT {expr};" for _, expr, _ in cases]
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
        f.write("\n".join(sql) + "\n")
        path = f.name
    try:
        r = subprocess.run([DUCKDB, "-unsigned", "-noheader", "-list"],
                           stdin=open(path), capture_output=True, text=True)
    finally:
        os.unlink(path)
    if r.returncode != 0 or "Error" in r.stderr:
        print("FAILED: could not run the rules.\n" + (r.stderr.strip() or r.stdout.strip()))
        return 1

    got = [l for l in r.stdout.strip().splitlines() if l.strip()]
    if len(got) != len(cases):
        print(f"FAILED: expected {len(cases)} results, got {len(got)}:\n{r.stdout}")
        return 1

    bad = 0
    for (name, _, want), actual in zip(cases, got):
        ok = actual.strip() == want
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"   want {want!r}, got {actual!r}"))
        bad += not ok
    print()
    # No exceptions any more: the level-0 frontmatter row was the last one, ruled
    # and fixed 2026-09-01. Saying "with the one recorded exception" after it was
    # gone would be a summary line claiming a caveat that no longer exists, which
    # is the same defect as claiming a guard that does not.
    print(f"OK: this reader conforms on all {len(cases)} assertions, no exceptions."
          if not bad else f"FAILED: {bad} conformance assertion(s).")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
