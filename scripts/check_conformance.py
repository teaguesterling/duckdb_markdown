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
import glob
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
# WATCHED, 2026-09-01, both override paths, because they were added for CI and
# nothing local had ever taken them:
#   MARKDOWN_EXTENSION=<v1.5.4 CI artifact>  -> loads, 18/18, rc=0
#   DUCKDB_BIN=<released v1.5.4 CLI>         -> the local dev extension will not
#                                               load into it, and this FAILS rc=1
# The second is the mismatch shape duck_block_utils hit, where an extension path
# pinned to one build and a binary falling through to another made the check SKIP
# silently. It cannot skip here -- there is no fallback and no skip path -- but
# the mismatch was fired rather than reasoned about.
#
# Note the asymmetry is real and not a bug: a v1.5.4 markdown artifact DOES load
# into this dev build, while duck_block_utils' extension does not (it is stamped
# v1.5.5 and this reports b155d6f63c). That is why check_against_producer.py
# still needs its JSON bridge and this check does not.
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



def corpus_cases():
    """Every markdown document in the repo, through the advisory rules.

    The CASES above are hand-built shapes; this is what the reader actually does
    to real documents. duck_blocks_warnings() is upstream's advisory set -- the
    preferences that validity does not express, so that two conformant producers
    cannot silently differ on the same document. duck_blocks_are_valid() also
    carries the two DOCUMENT-shape rules a per-element check cannot see: unique
    element_order, and depth descending one level at a time. Upstream calls the
    level-jump rule the one whose absence caused a year of drift across four
    extensions.

    Reported as the LIST OF OFFENDING FILES rather than a count, so a failure
    names the document instead of asserting that some document somewhere is bad.
    """
    corpus = sorted(glob.glob(os.path.join(REPO, "test/data/*.md")) +
                    glob.glob(os.path.join(REPO, "docs/*.md")) +
                    [os.path.join(REPO, "README.md")])
    corpus = [f for f in corpus if os.path.exists(f)]
    if not corpus:
        return []
    def lit(p):
        return "'" + p.replace("'", "''") + "'"
    warn = " UNION ALL ".join(
        f"SELECT {lit(os.path.relpath(f, REPO))} AS f FROM duck_blocks_warnings("
        f"(SELECT parse_markdown_to_duck_blocks(content) FROM read_text({lit(f)})))"
        for f in corpus)
    invalid = " UNION ALL ".join(
        f"SELECT {lit(os.path.relpath(f, REPO))} AS f FROM read_text({lit(f)}) "
        f"WHERE NOT duck_blocks_are_valid(parse_markdown_to_duck_blocks(content))"
        for f in corpus)
    return [
        (f"no advisory warning on any of {len(corpus)} repo documents",
         f"(SELECT coalesce(list(DISTINCT f), []) FROM ({warn}))", "[]"),
        (f"all {len(corpus)} repo documents are valid (order + level shape)",
         f"(SELECT coalesce(list(DISTINCT f), []) FROM ({invalid}))", "[]"),
    ]



def container_cases():
    """Every declared type as a container with TWO block children.

    Two, not one, and with a LEAD child before the probe: a walk that stops after
    the first child passes a single-child probe and looks correct. That is how a
    definition list dropped everything after its first child upstream for as long
    as the shape had existed, and how this repo's blockquote concatenated its
    children unnoticed.

    Checks both failure modes the container defects here took: children JOINED
    with no separator (ALPHA + BETA -> ALPHABETA, which a words-survive assertion
    cannot see) and children DROPPED.
    """
    m = re.search(r"duck_block_declared_types\(\) AS \(\s*\[(.*?)\]",
                  open(RULES).read(), re.S)
    types = re.findall(r"'([^']+)'", m.group(1)) if m else []
    if not types:
        return []
    def probe(t):
        return ("duck_blocks_to_md(["
                f"{{kind:'block',element_type:'{t}',content:NULL,level:1,encoding:'text',"
                "attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:0},"
                "{kind:'block',element_type:'paragraph',content:'ALPHA',level:2,encoding:'text',"
                "attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:1},"
                "{kind:'block',element_type:'paragraph',content:'BETA',level:2,encoding:'text',"
                "attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:2}])")
    union = " UNION ALL ".join(f"SELECT '{t}' AS t, {probe(t)} AS out" for t in types)
    # CONTROL: the detector must fire on output that really is joined. Without
    # it, a probe that silently matched nothing would read as "no type joins its
    # children" -- a pass meaning the detection is broken, not the code correct.
    control = ("(SELECT coalesce(list(t), []) FROM (SELECT 'x' AS t, "
               "duck_blocks_to_md([{kind:'block',element_type:'paragraph',content:'ALPHABETA',"
               "level:1,encoding:'text',attributes:MAP{}::MAP(VARCHAR,VARCHAR),element_order:0}]) AS out) "
               "WHERE out LIKE '%ALPHABETA%')")
    return [
        ("CONTROL the join detector fires on joined output", control, "[x]"),
        (f"no declared type JOINS its two block children ({len(types)} types)",
         f"(SELECT coalesce(list(t), []) FROM ({union}) WHERE out LIKE '%ALPHABETA%')", "[]"),
        (f"no declared type DROPS either block child ({len(types)} types)",
         f"(SELECT coalesce(list(t), []) FROM ({union}) WHERE out NOT LIKE '%ALPHA%' OR out NOT LIKE '%BETA%')", "[]"),
    ]


def main():
    for what, ok in (("this extension's build", os.path.exists(EXT)),
                     ("the vendored rules at conformance/", os.path.exists(RULES))):
        if not ok:
            print(f"FAILED: missing {what}. This check has no skip path by design.")
            return 1

    cases = CASES + absorption_cases() + container_cases() + corpus_cases()

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
