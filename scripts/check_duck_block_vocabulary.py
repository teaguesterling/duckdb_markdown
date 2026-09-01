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
LOCAL_CANDIDATES = [
    HEADER_REL,                                     # vendored (this repo)
    "third_party/duck_block_utils/" + HEADER_REL,   # submodule, if one is ever used
]
UPSTREAM_URL = ("https://raw.githubusercontent.com/teaguesterling/"
                "duckdb_duck_block_utils/main/" + HEADER_REL)
CONST_RE = re.compile(r'static\s+constexpr\s+[\w:*\s]+?\**(\w+)\s*=\s*(?:"([^"]*)"|([0-9]+))\s*;')

# Vocabulary we deliberately render through a fallthrough rather than a branch.
# Recorded here so the check stays silent about them and loud about everything
# else -- an unexplained gap and an intentional one look identical otherwise.
INTENTIONAL_FALLTHROUGH = {
    "VALUE_STRING": "RenderMetaValue's else arm takes content verbatim, which is "
                    "also the right handling for any value type this build "
                    "does not know",
}


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
            with urllib.request.urlopen(UPSTREAM_URL, timeout=30) as response:
                return response.read().decode("utf-8")
        except Exception as exc:
            sys.exit(f"error: cannot fetch {UPSTREAM_URL}\n{exc}\n"
                     f"       pass --upstream PATH to compare against a local clone instead")
    try:
        return subprocess.check_output(
            ["git", "-C", source, "show", f"{ref}:{HEADER_REL}"],
            stderr=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: cannot read {HEADER_REL} from {source}@{ref}\n{exc.stderr.strip()}")


def handled_types(src):
    """element_type / kind values our renderers actually branch on."""
    text = open(src).read()
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
    args = ap.parse_args()

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
    upstream = parse_constants(read_upstream(upstream_repo, args.ref))
    if not upstream:
        sys.exit("error: parsed no constants from upstream -- has the header's shape changed?")

    print(f"local    {os.path.relpath(local_path, root)}  ({len(local)} constants)")
    origin = f"{upstream_repo}@{args.ref}" if upstream_repo else UPSTREAM_URL
    print(f"upstream {origin}  ({len(upstream)} constants)")
    print(f"spec     local {local.get('SPEC_VERSION','?')}  upstream {upstream.get('SPEC_VERSION','?')}")
    print()

    removed = sorted(set(local) - set(upstream))
    added = sorted(set(upstream) - set(local))
    changed = sorted(k for k in set(local) & set(upstream) if local[k] != upstream[k])

    breaking = False
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
        print("vocabulary is in sync")

    # Gaps: types upstream publishes that no renderer branches on.
    named, literal = handled_types(os.path.join(root, "src/duck_block_functions.cpp"))
    vocab_types = {k: v for k, v in upstream.items()
                   if k.startswith(("TYPE_", "INLINE_", "VALUE_", "KIND_"))}
    gaps = sorted(k for k, v in vocab_types.items()
                  if k not in named and v not in literal and k not in INTENTIONAL_FALLTHROUGH)
    if gaps:
        print()
        print("GAPS   published but not branched on (renders via fallthrough):")
        for k in gaps:
            print(f"         {k} = {vocab_types[k]!r}")

    print()
    if breaking:
        print("FAILED: vocabulary drift is breaking. Update the copy and the references.")
        return 1
    if added:
        print("OK with news: upstream added vocabulary. Review whether we should handle it.")
    else:
        print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
