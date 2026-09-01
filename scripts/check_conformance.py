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
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT = os.path.join(REPO, "build/release/extension/markdown/markdown.duckdb_extension")
DUCKDB = os.path.join(REPO, "build/release/duckdb")
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
    # The remaining half of the open decision: level 0 versus structural depth
    # (>= 1). This is the ONLY thing keeping a frontmatter document invalid, and
    # it is Teague's call, not a defect to patch at the point of emission.
    ("a frontmatter document is non-conformant as emitted",
     r"duck_blocks_are_valid(parse_markdown_to_duck_blocks(e'---\ntitle: T\n---\n\nB.\n'))",
     "false"),
    # ...and level is the SOLE cause, proven rather than asserted: raise level to
    # the conformant minimum and change NOTHING else, and the document validates.
    # Without this, the line above would still pass if a second, unrelated defect
    # appeared -- "invalid" is not evidence of WHY.
    ("...and raising level to 1 alone makes it conformant",
     r"""duck_blocks_are_valid([{kind: x.kind, element_type: x.element_type, content: x.content, level: greatest(x.level, 1), encoding: x.encoding, attributes: x.attributes, element_order: x.element_order} for x in parse_markdown_to_duck_blocks(e'---\ntitle: T\n---\n\nB.\n')])""",
     "true"),
]


def main():
    for what, ok in (("this extension's build", os.path.exists(EXT)),
                     ("the vendored rules at conformance/", os.path.exists(RULES))):
        if not ok:
            print(f"FAILED: missing {what}. This check has no skip path by design.")
            return 1

    sql = [f"LOAD '{EXT}';", f".read {RULES}", ".mode list", ".header off"]
    sql += [f"SELECT {expr};" for _, expr, _ in CASES]
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
    if len(got) != len(CASES):
        print(f"FAILED: expected {len(CASES)} results, got {len(got)}:\n{r.stdout}")
        return 1

    bad = 0
    for (name, _, want), actual in zip(CASES, got):
        ok = actual.strip() == want
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"   want {want!r}, got {actual!r}"))
        bad += not ok
    print()
    print("OK: this reader conforms, with the one recorded exception."
          if not bad else f"FAILED: {bad} conformance assertion(s).")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
