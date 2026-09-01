#!/usr/bin/env python3
"""Render real duck_block_utils output through this extension's writer.

WHY THIS EXISTS SEPARATELY FROM THE TEST SUITE.

test/sql/duck_block_*.test replay producer output as struct literals, on
purpose: a test that depends on another extension being installed skips
silently when it is not, and a silent skip is zero coverage wearing the
costume of some. Those tests can never skip.

But literals only ever contain shapes someone already knew about. Every
content-loss defect found on 2026-08-31 -- a list_item's text dropped, a
blockquote rendered as raw Pandoc AST, a whole list rendering as nothing --
was found by running a LIVE producer through the writer and looking, and none
of them could have been found by the suite. So this is the other half, kept
runnable rather than performed once by hand.

WHY IT IS A BRIDGE RATHER THAN A LOAD. DuckDB matches extension ABI on the
exact version string, and this repo builds DuckDB from a pinned submodule --
so its binary reports a dev hash and REFUSES a release-built extension
outright. That is permanent, not a symptom of duck_block_utils being
unpublished: any consumer vendoring a duckdb submodule needs this shape
forever. So the producer runs in the system duckdb, this extension's writer
runs in this repo's build, and JSON crosses between them.

Usage:
    python3 scripts/check_against_producer.py [--producer PATH] [--pandoc]

Exits 1 on a rendering that loses content, 0 otherwise. Skips LOUDLY, never
silently, when the producer is unavailable.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT_MD = os.path.join(REPO, "build/release/extension/markdown/markdown.duckdb_extension")
MD_DUCKDB = os.path.join(REPO, "build/release/duckdb")
DEFAULT_PRODUCER = os.path.expanduser(
    "~/Projects/duckdb_duck_block_utils/build/release/extension/"
    "duck_block_utils/duck_block_utils.duckdb_extension")
SCHEMA = ("STRUCT(kind VARCHAR, element_type VARCHAR, content VARCHAR, level INTEGER, "
          "encoding VARCHAR, attributes MAP(VARCHAR,VARCHAR), element_order INTEGER)")

# Documents to push through, with the words that must survive the trip.
# Round trips that are KNOWN not to be stable, with the reason. Recorded rather
# than silently tolerated: an unexplained exclusion and a forgotten defect look
# identical later. Anything NOT listed here that drifts is a failure.
#
# ENTRIES EXPIRE. `line block` lived here and was removed once upstream fixed
# the break constructors -- keeping it would have hidden the next regression
# behind an explanation that had stopped being true. Re-read the reason before
# trusting an entry; the reason is what tells you it has expired, which is why
# these record WHY rather than merely THAT.
#
# WHAT THIS ARM DOES AND DOES NOT CATCH. It compares pass one against pass two,
# so it finds output that MEANS something different when read back -- a
# definition continuation indented two columns instead of four, a caption that
# duplicates on every pass. It does NOT catch output that is consistently
# wrong: rendering a hard break as a soft one round-trips perfectly, because
# the degraded form is self-consistent. Verified by perturbation in both
# directions. Content loss is caught by the word check below; consistent
# wrongness is what the sqllogictest suite is for. Three arms, three
# properties, and none of them subsumes another.
# WATCHED, 2026-09-01. Nothing in a green run can show that the audit below
# works: this dict is empty, so both of its loops iterate over nothing and the
# branches never execute. An audit nobody has seen run is the same evidence as no
# audit and it presents better -- duck_block_utils found a staleness branch that
# had never executed once while every green run printed that it had passed.
#
# So both were fired, and this is the record:
#   {"heading": ...}       -> EXPIRED heading: ... its round trip is now STABLE
#   {"no_such_case": ...}  -> EXPIRED no_such_case: ... no longer has a case
# Both exit non-zero. Re-plant either to re-watch it.
#
# This version at least stays quiet when empty rather than asserting its own
# verdict, so a green run claims nothing it cannot back.
KNOWN_UNSTABLE = {
}

# (name, markdown, words that must survive, [substrings that must NOT appear]).
#
# The fourth field exists because the third CANNOT see a lost separator:
# "quoted parasecond para" contains both "quoted" and "second", so a
# words-survive assertion passes on output where two paragraphs were
# concatenated. Forbidding the join is the assertion that matches the property.
#
# Worth being exact about what this did and did not miss. This check runs
# duck_block_utils' output through THIS writer, and that path was always
# correct -- the concatenation defect was in this repo's READER, which this
# check never touches. So the coarse assertion did not hide that bug; a missing
# DIRECTION did (covered now by duck_block_container_children.test). The
# coarseness was a real latent weakness on the writer side, and is closed here
# rather than left because it happened not to be the one that bit.
CASES = [
    ("heading",        "# Title\n",                              ["Title"]),
    ("nested inlines", "Text with **bold *inner*** here.\n",     ["bold", "inner"]),
    ("bullet list",    "- alpha\n- beta\n",                      ["alpha", "beta"], ["alphabeta"]),
    ("ordered list",   "3. one\n4. two\n",                       ["one", "two"], ["onetwo"]),
    ("blockquote",     "> quoted para\n>\n> second para\n",      ["quoted", "second"],
                       ["parasecond"]),
    ("table",          "| A | B |\n|---|---|\n| 1 | 2 |\n",      ["A", "B", "1", "2"]),
    # Multi-block on purpose: a definition's continuation block must be indented
    # far enough that a reader keeps it inside the definition. At two spaces it
    # reads as a new top-level paragraph, which splits the list and strands the
    # block between the halves -- caught by the round trip, not by rendering.
    ("definition list","HTTP\n:   hypertext transfer protocol\n\nTLS\n:   transport layer security\n\n    a second block\n\nDNS\n:   sense one\n:   sense two\n",
                       ["HTTP", "hypertext", "TLS", "transport", "a second block", "DNS", "sense one", "sense two"]),
    ("line block",     "| Roses are red\n| Violets are blue\n",  ["Roses", "Violets"]),
    ("figure",         "![A caption](img.png)\n",                ["img.png", "A caption"]),
    ("code",           "```py\nprint(1)\n```\n",                 ["print(1)"]),
]


def sql(binary, text, unsigned=True):
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
        f.write(text)
        path = f.name
    try:
        argv = [binary] + (["-unsigned"] if unsigned else []) + ["-noheader", "-list"]
        r = subprocess.run(argv, stdin=open(path), capture_output=True, text=True)
        if r.returncode != 0 or r.stderr.strip():
            raise RuntimeError((r.stderr or "rc=%d" % r.returncode).strip())
        return r.stdout.strip()
    finally:
        os.unlink(path)


def to_blocks(producer, markdown):
    """markdown -> pandoc AST -> duck_blocks, via the producer."""
    ast = subprocess.run(["pandoc", "-f", "markdown", "-t", "json"],
                         input=markdown, capture_output=True, text=True).stdout.strip()
    q = ast.replace("'", "''")
    return sql("duckdb", f"LOAD '{producer}';\nSELECT to_json(pandoc_ast_to_blocks('{q}'));\n")


def render(blocks_json):
    """duck_blocks -> markdown, via this repo's writer."""
    q = blocks_json.replace("'", "''")
    return sql(MD_DUCKDB,
               f"LOAD '{EXT_MD}';\n"
               f"SELECT duck_blocks_to_md(from_json('{q}'::JSON, '[\"{SCHEMA}\"]'));\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--producer", default=os.environ.get("DUCK_BLOCK_UTILS", DEFAULT_PRODUCER))
    args = ap.parse_args()

    for what, ok in (("this extension's build", os.path.exists(EXT_MD)),
                     ("a system `duckdb` on PATH", shutil.which("duckdb")),
                     ("`pandoc` on PATH", shutil.which("pandoc")),
                     (f"a duck_block_utils build at {args.producer}", os.path.exists(args.producer))):
        if not ok:
            # Loudly. A silent skip here would report health it never checked.
            print(f"SKIPPED: needs {what}.")
            print("         Nothing was verified. This is not a pass.")
            return 0

    # Asymmetric pre-flight: put a shape through the bridge whose answer is
    # already known, and specifically one it MUST fail on. A bridge that cannot
    # object returns silence indistinguishable from a clean bill of health, and
    # the quiet direction is the one that ships.
    control = json.dumps([{"kind": "block", "element_type": "paragraph", "content": "CONTROLWORD",
                           "level": 1, "encoding": "text", "attributes": {}, "element_order": 0}])
    if "CONTROLWORD" not in render(control):
        print("BRIDGE BROKEN: the control document did not survive rendering.")
        print("               Every result below would be meaningless; not run.")
        return 1

    failures, expired = [], []
    for case in CASES:
        name, markdown, expected = case[0], case[1], case[2]
        forbidden = case[3] if len(case) > 3 else []
        out = render(to_blocks(args.producer, markdown))
        missing = [w for w in expected if w not in out]
        joined = [w for w in forbidden if w in out]
        # Rendering once proves the words survive; rendering the OUTPUT again
        # proves the markdown says what it meant. A definition continuation
        # indented too shallowly renders fine and re-reads as a different
        # document, which only the second pass can see.
        second = render(to_blocks(args.producer, out)) if out.strip() else out
        unstable = second.strip() != out.strip()
        known = name in KNOWN_UNSTABLE
        if known and not unstable:
            expired.append(name)
        if joined:
            status = "JOIN"
        elif missing:
            status = "LOST"
        elif unstable:
            status = "note" if known else "DRIFT"
        else:
            status = "ok  "
        print(f"  {status} {name:<16} {out.splitlines()[0][:42] if out.strip() else '(empty)'}")
        if joined or missing or (unstable and not known):
            detail = (f"joined across a block boundary: {joined}\n{out!r}" if joined
                      else out if missing else f"unstable:\n{out!r}\n{second!r}")
            failures.append((name, missing or joined, detail))

    print()

    # AUDIT THE EXCLUSIONS. An entry whose case now passes has expired, and an
    # expired exclusion is worse than none: it goes on excusing a case that no
    # longer needs it and hides the next regression behind an explanation
    # nobody rechecks. `line block` lived here and expired within an hour of
    # being written, caught only because it happened to be re-measured.
    #
    # This FAILS rather than warns, because the instinct on meeting a stale
    # reason is to fix the wording, and the notice arrives exactly when you are
    # least inclined to delete something you wrote. The fix is to delete the
    # entry, not to reword it. (Adopted from duck_block_utils, who built the
    # same audit over their sweep's exemption registries.)
    dead = [n for n in KNOWN_UNSTABLE if n not in {c[0] for c in CASES}]
    for name in expired:
        print(f"EXPIRED {name}: listed as known-unstable, but its round trip is now STABLE.")
        print(f"         DELETE the entry -- do not reword it. Recorded reason was:")
        print(f"         {KNOWN_UNSTABLE[name]}")
    for name in dead:
        print(f"EXPIRED {name}: listed as known-unstable but no longer has a case. DELETE it.")
    if expired or dead:
        print()

    for name in sorted(n for n in {c[0] for c in CASES} if n in KNOWN_UNSTABLE and n not in expired):
        print(f"note {name}: round trip is known-unstable -- {KNOWN_UNSTABLE[name]}")
    if KNOWN_UNSTABLE:
        print()
    if expired or dead:
        print("FAILED: the exclusion list is out of date.")
        return 1
    if failures:
        for name, missing, out in failures:
            print(f"FAILED {name}: lost {missing}\n       rendered: {out!r}")
        return 1
    print(f"OK: {len(CASES)} documents rendered from live producer output, no content lost.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
