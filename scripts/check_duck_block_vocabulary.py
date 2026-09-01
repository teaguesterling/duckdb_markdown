#!/usr/bin/env python3
"""Check this extension's duck_block vocabulary against upstream.

A submodule pin and a vendored copy have the same defect: neither notices when
upstream moves. This closes that, and reports two different things:

  DRIFT    a constant upstream renamed, removed, or changed the value of.
           Breaking -- our string literals silently stop matching.
  GAPS     an element type upstream publishes that our renderers never branch
           on. Not breaking, but it renders through a fallthrough, which is how
           inline `generic` came to drop its source_type silently.

Constants are compared by name and value, not by diffing text, so churn that
does not change the vocabulary (idx_t -> uint64_t, comments, formatting) stays
quiet. A check that cries wolf gets ignored, and then it is worth nothing.
"""
import argparse
import os
import re
import subprocess
import sys
import urllib.request

HEADER_REL = "src/include/duck_block_vocabulary.hpp"
CONFORMANCE_REL = "conformance/duck_block_conformance.sql"
LOCAL_CANDIDATES = [
    HEADER_REL,                                     # vendored (this repo)
    "third_party/duck_block_utils/" + HEADER_REL,   # submodule, if one is ever used
]
UPSTREAM_OWNER = "teaguesterling"
UPSTREAM_REPO = "duckdb_duck_block_utils"
UPSTREAM_BRANCH = "main"


def _get(url, accept=None):
    headers = {"Accept": accept} if accept else {}
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    return urllib.request.urlopen(
        urllib.request.Request(url, headers=headers), timeout=30).read().decode("utf-8")


def fetch_upstream_header():
    """Fetch the upstream header, defeating the raw-endpoint CDN cache.

    raw.githubusercontent.com serves a BRANCH url from a cache that can lag the
    branch by minutes. That produced a false "in sync" here against a copy two
    spec versions old -- a false negative in the reassuring direction, from the
    tool whose whole job is to catch exactly that. So resolve the branch to a
    commit sha first and fetch the sha-pinned url, which is immutable and
    therefore never stale. Returns (text, provenance, verified).
    """
    raw = "https://raw.githubusercontent.com/{}/{}/{{}}/{}".format(
        UPSTREAM_OWNER, UPSTREAM_REPO, HEADER_REL)
    try:
        sha = _get("https://api.github.com/repos/{}/{}/commits/{}".format(
            UPSTREAM_OWNER, UPSTREAM_REPO, UPSTREAM_BRANCH),
            accept="application/vnd.github.sha").strip()
        return _get(raw.format(sha)), "{}@{}".format(UPSTREAM_BRANCH, sha[:12]), True
    except Exception as exc:
        # Fall back to the branch url so an API outage or rate limit does not
        # block work -- but never report this result as verified.
        text = _get(raw.format(UPSTREAM_BRANCH))
        return text, "{} (branch url; sha lookup failed: {})".format(UPSTREAM_BRANCH, exc), False
CONST_RE = re.compile(r'static\s+constexpr\s+[\w:*\s]+?\**(\w+)\s*=\s*(?:"([^"]*)"|([0-9]+))\s*;')

# Vocabulary we deliberately render through a fallthrough rather than a branch.
# Recorded here so the check stays silent about them and loud about everything
# else -- an unexplained gap and an intentional one look identical otherwise.
INTENTIONAL_FALLTHROUGH = {
    "VALUE_STRING": "RenderMetaValue's else arm takes content verbatim, which is "
                    "also the right handling for any value type this build "
                    "does not know",
    "TYPE_PLAIN": "a text run with no paragraph semantics; the unknown-block "
                  "terminal arm already renders it exactly as a paragraph, "
                  "verified at top level, in a tight list item and inside a "
                  "container. A branch would restate the fallback",
}


def parse_version(text):
    """MAJOR.MINOR -> (major, minor); (None, None) when unparseable."""
    m = re.match(r"^\s*(\d+)\.(\d+)", text or "")
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def compare_spec_version(local, upstream):
    """Apply the vocabulary's stated version contract.

    MAJOR bumps for a breaking change, MINOR for an additive one, and the header
    says to assert MAJOR equality plus a MINOR floor rather than equality on the
    whole string -- equality goes red on releases that cannot affect us, and a
    check that cries wolf gets muted.

    Returns (breaking, note). Note that upstream's own recorded history has one
    counterexample: 1.1 -> 1.2 was breaking and shipped as a minor, and it is
    what broke this writer in three places. The contract holds from 2.0 onward,
    so a minor bump is reported loudly rather than silently, but not failed.
    """
    lo, up = parse_version(local), parse_version(upstream)
    if lo == (None, None) or up == (None, None):
        return True, f"SPEC_VERSION unparseable (local {local!r}, upstream {upstream!r})"
    if lo[0] != up[0]:
        return True, (f"MAJOR changed {local} -> {upstream}: a breaking shape or "
                      f"vocabulary change this build has not migrated for")
    if up[1] > lo[1]:
        return False, (f"MINOR ahead {local} -> {upstream}: additive by contract, so "
                       f"nothing here should break -- review and re-vendor when convenient")
    if up[1] < lo[1]:
        return True, (f"MINOR behind {local} -> {upstream}: this copy claims a newer "
                      f"spec than upstream publishes")
    return False, None


def check_vendored_conformance(root, vocab):
    """The conformance SQL embeds the type list -- a SECOND copy of the vocabulary.

    Two copies checked by the same party cannot detect their own disagreement, so
    the embedded list is compared against the vendored header here. The header is
    compared against upstream above, so drift in either link fails. Without this,
    adding a type upstream would make the vendored SQL report it as undeclared --
    a false positive from a check whose whole job is telling truth from typo.
    """
    path = os.path.join(root, CONFORMANCE_REL)
    if not os.path.exists(path):
        return []
    text = open(path).read()
    problems = []
    # Kinds and element types are DIFFERENT AXES and are compared separately.
    # Conflating them is not hypothetical: this check's first version compared the
    # header's whole vocabulary against the types list alone and reported block,
    # inline and value as missing. The file exported only one axis then, which is
    # what made the mistake available; both are exported now.
    for label, macro, expected in (
        ("kinds", "duck_block_declared_kinds", vocab["kinds"]),
        ("types", "duck_block_declared_types", vocab["types"]),
        # Encodings became a THIRD axis upstream on 2026-09-01, after the list had
        # been hardcoded in four places and nothing compared them. A new list that
        # nothing compares is the same gap that let `role` go unguarded here, so it
        # is compared from the day it appears rather than the day it drifts.
        ("encodings", "duck_block_declared_encodings", vocab["encodings"]),
    ):
        m = re.search(r"MACRO " + macro + r"\(\) AS \(\s*\[(.*?)\]", text, re.S)
        if not m:
            problems.append(f"{CONFORMANCE_REL}: no {macro}() list to compare")
            continue
        entries = re.findall(r"'([^']+)'", m.group(1))
        embedded = set(entries)
        # MULTIPLICITY, checked before the set comparison that would hide it.
        # Building a set() is the coarser measurement: a list that names `code`
        # twice compares equal to one that names it once, so len() lies and any
        # join against it double-counts. duck_block_utils shipped exactly that --
        # duck_block_type_names() returned 47 rows for a 43-type vocabulary --
        # and every check there built a set() from it, so nothing could see it.
        # Five names legitimately live on two AXES here (code, generic, image,
        # raw as block and inline; list as block and value), which is why the
        # header has more constants than names and why a duplicate in a flat
        # list is easy to introduce and invisible to compare.
        duped = sorted({v for v in entries if entries.count(v) > 1})
        if duped:
            problems.append(f"{CONFORMANCE_REL}: {label} listed more than once: {duped}")
        missing = sorted(expected - embedded)
        extra = sorted(embedded - expected)
        if missing:
            problems.append(f"{CONFORMANCE_REL} is MISSING {label} the header declares: {missing}")
        if extra:
            problems.append(f"{CONFORMANCE_REL} lists {label} the header does not: {extra}")
    return problems


def vocabulary_of(constants):
    """The element-type constants, excluding struct field offsets.

    Selecting on the name prefix alone also catches KIND_IDX, which is a field
    offset rather than an element type -- it reported as a GAP, which is the
    noise that teaches people to ignore the arm that earns its keep. Vocabulary
    values are lowercase tokens; offsets are digits. (Found by panduck.)
    """
    return {k: v for k, v in constants.items()
            if k.startswith(("TYPE_", "INLINE_", "VALUE_", "KIND_")) and not v.isdigit()}


def classify(local, upstream):
    """Split the comparison into removed / changed / added, by name AND value."""
    removed = sorted(set(local) - set(upstream))
    added = sorted(set(upstream) - set(local))
    changed = sorted(k for k in set(local) & set(upstream) if local[k] != upstream[k])
    return removed, changed, added


def self_test():
    """Pin the properties this check exists for, rather than asserting them in a
    comment. A guard nobody has watched fail is not yet a guard."""
    base = {"TYPE_PAGE": "page_break", "TYPE_FIGURE": "figure", "KIND_IDX": "0"}
    failures = []

    # A rename, with the constant COUNT held identical.
    renamed = {"TYPE_PAGE_BREAK": "page_break", "TYPE_FIGURE": "figure", "KIND_IDX": "0"}
    removed, changed, added = classify(base, renamed)
    if len(base) != len(renamed) or removed != ["TYPE_PAGE"] or not added:
        failures.append("rename not caught with the count held constant")

    # A value change, count identical, names identical.
    revalued = dict(base, TYPE_PAGE="pagebreak")
    removed, changed, added = classify(base, revalued)
    if len(base) != len(revalued) or changed != ["TYPE_PAGE"] or removed or added:
        failures.append("value change not caught with names and count identical")

    # Cosmetic churn: same names, same values, different text. Must be silent.
    removed, changed, added = classify(base, dict(base))
    if removed or changed or added:
        failures.append("cosmetic churn was not silent")

    # The version contract: major equality, minor floor.
    for lo, up, want_breaking in (("2.0", "2.0", False), ("2.0", "2.1", False),
                                  ("2.0", "3.0", True), ("2.0", "1.2", True),
                                  ("2.1", "2.0", True)):
        got, _ = compare_spec_version(lo, up)
        if got != want_breaking:
            failures.append(f"spec {lo} -> {up}: expected breaking={want_breaking}, got {got}")

    # Field offsets are not vocabulary.
    if "KIND_IDX" in vocabulary_of(base):
        failures.append("KIND_IDX selected as an element type")

    for line in failures:
        print("SELF-TEST FAILED: " + line)
    print("self-test: " + ("FAILED" if failures else "all properties hold"))
    return 1 if failures else 0


def parse_constants(text):
    out = {}
    for name, sval, ival in CONST_RE.findall(text):
        out[name] = sval if ival == "" else ival
    return out


def find_local(root):
    for rel in LOCAL_CANDIDATES:
        path = os.path.join(root, rel)
        if os.path.exists(path):
            return path
    return None


def read_upstream(source, ref):
    """Read the upstream header, from a local clone or over HTTPS.

    Vendoring means there is no local upstream to read, so the network is the
    default. A local clone stays supported for working offline.
    """
    if source is None:
        try:
            return fetch_upstream_header()
        except Exception as exc:
            sys.exit(f"error: cannot fetch the upstream header\n{exc}\n"
                     f"       pass --upstream PATH to compare against a local clone instead")
    try:
        text = subprocess.check_output(
            ["git", "-C", source, "show", f"{ref}:{HEADER_REL}"],
            stderr=subprocess.PIPE, text=True)
        return text, f"{source}@{ref}", True
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: cannot read {HEADER_REL} from {source}@{ref}\n{exc.stderr.strip()}")


def strip_comments(text):
    """Remove // and /* */ so a MENTION is not mistaken for a use.

    This scan asks which vocabulary constants the renderers branch on, and the
    answer feeds the fallthrough audit -- so a constant named only in a comment
    would read as handled and SUPPRESS a real gap. That is the same
    loose-pattern failure duck_block_utils hit measuring their own header, where
    a `TYPE_[A-Z_]+` matched the header's own cautionary example and manufactured
    four phantom findings. Theirs invented findings; this direction hides them,
    which is quieter and therefore worse to leave to luck.

    Measured when this was written: zero comment-only mentions across all three
    sources, so nothing changes today. It is the next explanatory comment naming
    a constant that this is for.
    """
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def handled_types(src):
    """element_type / kind values our renderers actually branch on."""
    text = strip_comments(open(src).read())
    named = set(re.findall(r'Vocab::([A-Z_]+)', text))
    literal = set(re.findall(r'element_type == "([a-z_:]+)"', text))
    return named, literal


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", default=None,
                    help="path to a duck_block_utils clone (default: fetch over HTTPS)")
    ap.add_argument("--ref", default="origin/main",
                    help="upstream ref to compare against (default: origin/main)")
    ap.add_argument("--fetch", action="store_true", help="git fetch the upstream ref first")
    ap.add_argument("--strict", action="store_true",
                    help="fail when the upstream copy could not be pinned to a sha "
                         "(for CI, where a green run that verified nothing is worse "
                         "than a red one)")
    ap.add_argument("--self-test", action="store_true",
                    help="check the checker: rename, value change and cosmetic churn")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    local_path = find_local(root)
    if not local_path:
        sys.exit("error: no duck_block_vocabulary.hpp found in " + " or ".join(LOCAL_CANDIDATES))

    upstream_repo = args.upstream
    if upstream_repo is not None:
        # A submodule's .git is a file pointing at the real gitdir, not a directory.
        if not os.path.exists(os.path.join(upstream_repo, ".git")):
            sys.exit(f"error: {upstream_repo} is not a git clone")
        if args.fetch:
            subprocess.run(["git", "-C", upstream_repo, "fetch", "--quiet", "origin"], check=False)

    local = parse_constants(open(local_path).read())
    upstream_text, provenance, verified = read_upstream(upstream_repo, args.ref)
    upstream = parse_constants(upstream_text)
    if not upstream:
        sys.exit("error: parsed no constants from upstream -- has the header's shape changed?")

    print(f"local    {os.path.relpath(local_path, root)}  ({len(local)} constants)")
    print(f"upstream {provenance}  ({len(upstream)} constants)")
    if not verified:
        print("WARNING  upstream copy is UNVERIFIED and may be a stale cache")
    print(f"spec     local {local.get('SPEC_VERSION','?')}  upstream {upstream.get('SPEC_VERSION','?')}")
    print()

    removed, changed, added = classify(local, upstream)
    # SPEC_VERSION is a version, not vocabulary: it has its own contract.
    changed = [k for k in changed if k != "SPEC_VERSION"]
    spec_breaking, spec_note = compare_spec_version(
        local.get("SPEC_VERSION", ""), upstream.get("SPEC_VERSION", ""))

    breaking = spec_breaking
    if spec_note:
        print(("SPEC   " if spec_breaking else "SPEC   ") + spec_note)
        print()
    if removed:
        breaking = True
        print("DRIFT  gone upstream (our references would no longer compile):")
        for k in removed:
            print(f"         {k} = {local[k]!r}")
    if changed:
        breaking = True
        print("DRIFT  value changed upstream (our output silently stops matching):")
        for k in changed:
            print(f"         {k}: {local[k]!r} -> {upstream[k]!r}")
    if added:
        print("NEW    published upstream, not in our copy:")
        for k in added:
            print(f"         {k} = {upstream[k]!r}")
    if not (removed or changed or added):
        print("vocabulary constants are in sync")

    # The vendored conformance SQL carries its own copy of the type list.
    vocab_axes = {
        "kinds": {v for k, v in vocabulary_of(local).items() if k.startswith("KIND_")},
        "types": {v for k, v in vocabulary_of(local).items() if not k.startswith("KIND_")},
        "encodings": {v for k, v in local.items() if k.startswith("ENCODING_") and not v.isdigit()},
    }
    for problem in check_vendored_conformance(root, vocab_axes):
        breaking = True
        print("DRIFT  " + problem)
        print("       Re-vendor conformance/duck_block_conformance.sql from upstream.")

    # Gaps: types upstream publishes that no renderer branches on.
    named, literal = handled_types(os.path.join(root, "src/duck_block_functions.cpp"))
    vocab_types = vocabulary_of(upstream)
    gaps = sorted(k for k, v in vocab_types.items()
                  if k not in named and v not in literal and k not in INTENTIONAL_FALLTHROUGH)

    # AUDIT THE ALLOWLIST, in BOTH ways an entry expires. An entry that excuses
    # nothing is worse than none: it reads as a considered decision while
    # guarding a case that no longer exists.
    #
    #   stale key   the constant was renamed or removed upstream, so the entry
    #               matches nothing. Invisible to any check that only asks
    #               whether the excused case still fails -- the case is simply
    #               absent, which looks exactly like the exemption working.
    #   superseded  the type is now branched on explicitly, so it is not a
    #               fallthrough at all. Filtered out before the allowlist is
    #               consulted, so it would never be reported either.
    #
    # (The first mode is duck_block_utils'; the second is the one they took
    # from here. Neither audit had both until tonight.)
    for key, reason in sorted(INTENTIONAL_FALLTHROUGH.items()):
        if key not in vocab_types:
            breaking = True
            print(f"EXPIRED  {key}: allowlisted as an intentional fallthrough, but upstream")
            print(f"         no longer publishes it. DELETE the entry -- it excuses nothing.")
            print(f"         Recorded reason was: {reason}")
        elif key in named or vocab_types[key] in literal:
            breaking = True
            print(f"EXPIRED  {key}: allowlisted as an intentional fallthrough, but this build")
            print(f"         now branches on it explicitly. DELETE the entry, do not reword it.")
            print(f"         Recorded reason was: {reason}")
    if gaps:
        print()
        print("GAPS   published but not branched on (renders via fallthrough):")
        for k in gaps:
            print(f"         {k} = {vocab_types[k]!r}")

    print()
    if breaking:
        print("FAILED: vocabulary drift is breaking. Update the copy and the references.")
        return 1
    if not verified:
        # Never claim a clean bill of health from a copy we could not date. Locally
        # this is a warning so an API outage does not block work; in CI it is a
        # failure, because a green check that verified nothing is worse than a red
        # one -- it is indistinguishable from a real pass by anyone reading the
        # badge, which is the whole hazard this script exists to avoid.
        print("UNVERIFIED: no drift seen, but the upstream copy could not be pinned to a sha.")
        if args.strict:
            print("FAILED: --strict, and this result is unverified.")
            return 1
        return 0
    if added:
        print("OK with news: upstream added vocabulary. Review whether we should handle it.")
    else:
        print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
