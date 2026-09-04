# Building a DuckDB extension against both v1.5 and v2.0

DuckDB `main` is the **v2.0** line (`v2.0-cyanoptera`). A major release breaks API
by definition, and `duckdb/community-extensions` builds every release PR against
it via `build_next.yml`:

```yaml
on:
  pull_request:
    paths-ignore: ['**', '!extensions/*/description.yml']
jobs:
  test_against_latest:
    with:
      duckdb_version: 'v2.0-cyanoptera'
```

That trigger fires on exactly a `description.yml` change — i.e. every release PR —
and **there is no per-extension opt-out**. So an extension that does not build
against v2.0 has a red check on every release, and in practice that blocks: of the
last eight PRs merged there, all eight had zero failures on that job.

Your extension still *ships* against the pinned stable DuckDB. The goal is source
that compiles under both, not a migration.

## Detect features, not versions

A version macro says *when* something changed. A probe says whether it changed
**here** — which keeps working when a change is backported, reverted, or lands on
a branch you did not expect.

Two probes cover everything below.

**Header presence**, for a type that is new:

```cpp
#if __has_include("duckdb/common/identifier.hpp")
#define DUCKDB_HAS_IDENTIFIER 1
#include "duckdb/common/identifier.hpp"
#endif
```

**Member presence**, for a method that changed shape. `if constexpr` only discards
the untaken branch inside a template, hence the template parameter:

```cpp
template <class T, class = void>
struct CompatHasWithAlias : std::false_type {};
template <class T>
struct CompatHasWithAlias<T, decltype(void(std::declval<const T &>().WithAlias(string())))>
    : std::true_type {};

template <class TYPE = LogicalType>
inline LogicalType CompatWithAlias(TYPE type, string alias) {
	if constexpr (CompatHasWithAlias<TYPE>::value) {
		return type.WithAlias(std::move(alias));   // v2.0: returns a copy
	} else {
		type.SetAlias(std::move(alias));           // v1.5: mutates in place
		return type;
	}
}
```

Probe each change **separately**. Tying several to one macro silently picks the
wrong branch if they ever land in different releases.

**Probe for the thing that exists only on the NEW line.** This is the rule that
is easy to get backwards, and getting it backwards fails silently. Our
`ToUnifiedFormat` shim probed for the *count-taking* overload — the one being
replaced. But v2.0 kept that overload as `[[deprecated]]` rather than deleting
it, so the probe was true on **both** versions and the shim always took the old
branch, never once reaching the new API it existed for. It compiled and behaved
correctly everywhere, which is why nothing caught it. Probing for the count-free
overload — which exists only on v2.0 — is what actually discriminates.

### Check your C++ standard before copying shims

`if constexpr` is C++17. Several extensions here compile their TUs at **C++11 on
purpose** — forcing C++17 on the extension but not on libduckdb makes
static-const members in duckdb's headers acquire implicit inline linkage in one
set of TUs and not the other, producing multiple-definition link errors. One line
tells you which you are:

```
grep -o 'std=c++[0-9]*' build/release/compile_commands.json | sort -u
```

The member probe itself is fine at C++11 in the `decltype(void(expr))` form. Only
the dispatch needs care, so write it as tag dispatch, which works at both
standards and has the same "only the taken branch is instantiated" property:

```cpp
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE t, string a, std::true_type) {
	return t.WithAlias(std::move(a));            // v2.0
}
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE t, string a, std::false_type) {
	t.SetAlias(std::move(a));                    // v1.5
	return t;
}
template <class TYPE = LogicalType>
inline LogicalType CompatWithAlias(TYPE t, string a) {
	return CompatWithAliasImpl(std::move(t), std::move(a), CompatHasWithAlias<TYPE>());
}
```

markdown's header is written this way, so it is safe to copy into a C++11 repo.

## The changes

### 1. `LogicalType::SetAlias` → `WithAlias`

`WithAlias` returns a copy rather than mutating a type whose type-info may be
shared. Use the `CompatWithAlias` shim above.

`SetAlias` is **removed**, not deprecated — on main the compiler says
"`struct duckdb::LogicalType` has no member named `SetAlias`; did you mean
`GetAlias`?". There is no grace period to plan around.

*Check while you are there:* `SetAlias` was a setter, so calling it twice
**replaced** the alias rather than adding one. If your code did that, only the
last call ever took effect.

### 2. `vector<string>` → `vector<Identifier>` in bind signatures

Affects `table_function_bind_t` and `copy_to_bind_t`. `Identifier` compares
case-insensitively, which is why it exists.

The conversion asymmetry is what makes this cheap:

```cpp
Identifier(const char *str);       // IMPLICIT — literals are identifiers by intent
explicit Identifier(const string&); // EXPLICIT — promoting a runtime string is deliberate
```

So `names.emplace_back("file_path")` compiles unchanged on both. **Only the
signatures move**, plus the few places a *runtime* string crosses the boundary:

```cpp
#ifdef DUCKDB_HAS_IDENTIFIER
using CompatName = Identifier;
inline string CompatNameStr(const Identifier &id) { return id.GetIdentifierName(); }
inline Identifier CompatMakeName(string n) { return Identifier(std::move(n)); }
#else
using CompatName = string;
inline string CompatNameStr(const string &n) { return n; }
inline string CompatMakeName(string n) { return n; }
#endif
```

Then `vector<string> &names` becomes `vector<CompatName> &names` in every bind
declaration *and* definition, and runtime boundaries use the helpers. Do not add
an implicit conversion — the explicitness is the upstream change's point.

It is not only bind parameters. COPY **option keys** are identifiers too, so
`option.first` needs the same helper. Grep for all of it before starting — and
note that the compiler will not show you the whole list:

```
grep -rn 'vector<string> &names\|vector<string> &return_names' src/
grep -rn 'option\.first\|names\[' src/
```

### 3. `FlatVector::GetData<T>` is read-only; writes need `GetDataMutable<T>`

```
v1.5:  GetData<T>(vec)         -> T*
v2.0:  GetData<T>(vec)         -> const T*
       GetDataMutable<T>(vec)  -> T*
```

Writing through the v2.0 read accessor is a compile error, which is the split's
purpose. Probe for the member and route the write path:

```cpp
template <class VALUE, class FV = FlatVector>
inline VALUE *CompatFlatDataMutable(Vector &vec) {
	if constexpr (CompatHasFlatGetDataMutable<FV>::value) {
		return FV::template GetDataMutable<VALUE>(vec);
	} else {
		return FV::template GetData<VALUE>(vec);
	}
}
```

### 4. `UnaryExecutor::ExecuteWithNulls` — removed

Also `BinaryExecutor::` and `TernaryExecutor::ExecuteWithNulls`. This is the
change most likely to alter behaviour quietly, and the first attempt at it in
this repo **did** — so read this section before writing a replacement.

**First, find out whether your lambda's null branch is even reachable.** In
`unary_executor.hpp`, `ExecuteWithNulls` copies the input validity into the
result mask and then *skips the lambda entirely* for null rows:

```cpp
result_mask.Copy(mask, count);          // result is NULL wherever input was
...
if (ValidityMask::RowIsValid(validity_entry, base_idx - start)) {
    result_data[base_idx] = OP::Operation(...);   // only called for VALID rows
}
```

So a lambda that opens with `if (!mask.RowIsValid(idx)) return X;` never runs
that branch, and the function has always returned **NULL** for a null input. The
`ValidityMask &` parameter is there so a function can *add* nulls on valid rows,
not so it can observe them.

**Therefore, if the branch is dead, a plain `Execute` is the exact equivalent** —
it propagates nulls identically, exists in both v1.5 and v2.0, and needs no shim:

```cpp
UnaryExecutor::Execute<string_t, bool>(input, result, count,
                                       [](string_t s) { return !s.GetString().empty(); });
```

Only if the lambda genuinely *produces* nulls on valid rows do you need v2.0's
`optional<RESULT_TYPE>` form, and then a shim like duckdb_yaml's
`CompatUnaryExecuteWithNulls` / `CompatBinaryExecuteWithNulls` is the way.

#### The trap

The obvious-looking "explicit loop" replacement is wrong, and it fails silently:

```cpp
// WRONG -- turns NULL into false for non-constant inputs
for (idx_t i = 0; i < count; i++) {
    const auto idx = vdata.sel->get_index(i);
    out[i] = vdata.validity.RowIsValid(idx) && !in[idx].GetString().empty();
}
```

It writes a *value* where the original left the row NULL, and it never touches
the result validity mask. Here is what that cost us, measured against the pinned
v1.5 by rebuilding one file:

```
SELECT v, md_valid(v) FROM (VALUES ('x'), (NULL), ('')) t(v);

pre-port    x -> true   NULL -> NULL    '' -> false
"ported"    x -> true   NULL -> FALSE   '' -> false     <- regression
fixed       x -> true   NULL -> NULL    '' -> false
```

**And note why it was missed.** `SELECT md_valid(NULL)` returns NULL in *all
three* versions, because constant folding propagates the null above the function
before it ever runs. Testing the constant makes the bug invisible. **Test a NULL
inside a non-constant vector** — `FROM (VALUES ...)` — or you are not testing the
code path you changed.

### 5. `FunctionSet<T>::functions` yields `shared_ptr<const T>`

A function set no longer hands out mutable references to its members, so a loop
that configures functions *after* adding them stops compiling:

```
error: 'class duckdb::shared_ptr<const duckdb::ScalarFunction>' has no member
       named 'SetStability'
```

There is no shim for this, and one would be wrong anyway: the fix is to finish
configuring each function **before** it goes into the set.

```cpp
// was: for (auto &f : set.functions) { f.SetStability(...); }   // no longer mutable
ScalarFunction f(...);
f.SetStability(FunctionStability::VOLATILE);   // configure first
set.AddFunction(f);                            // then add
```

Watch for this wherever a helper "post-processes" a whole set — null handling,
varargs and stability are the usual ones.

### 6. Public fields became private; accessors replace them

The largest class by error count, and the one a source grep will **not** find —
there is no distinctive token to search for. `ScalarFunction` and the expression
hierarchy closed their data members:

```
ScalarFunction::varargs        -> GetVarArgs() / SetVarArgs()
ScalarFunction::null_handling  -> GetNullHandling() / SetNullHandling()
ScalarFunction::return_type    -> GetReturnType() / SetReturnType()
Expression::return_type            (now protected)  -> GetReturnType()
BaseExpression::alias              (now protected, and now an Identifier)
BoundFunctionExpression::bind_info (now private)
```

`bind_scalar_function_t` also collapsed its three parameters into one input
object, and `Catalog::GetEntry`, `FunctionBinder::BindScalarFunction` and the
`FunctionExpression` constructor all take `Identifier` where they took `string`.

**Do not write these shims from scratch.** `duckdb_yaml`'s
`src/include/duckdb_compat.hpp` is the most complete in the fleet and already has
them — `DUCKDB_SCALAR_BIND_PARAMS` / `_CONTEXT` / `_ARGS`, `CompatExprReturnType`,
`CompatBoundChildren`, `CompatBoundBindInfo`, `CompatSetScalarReturnType`,
`CompatSetScalarNullHandling`, `CompatSetScalarVarArgs`, plus working
`CompatUnaryExecuteWithNulls` / `CompatBinaryExecuteWithNulls`. Start from that
header and add this document's v2.0 shims to it, rather than starting from a
smaller one and rediscovering the same helpers.

An extension that only *registers* functions barely touches this. One that
*introspects* the catalog or rewrites expressions (`func_apply` is the extreme
case) touches almost all of it.

### 7. Named-argument aliases are no longer captured — a SILENT runtime break

The most dangerous item here, because there is **no compile error and nothing to
grep for**. The build is green and the extension fails at runtime.

v1.5 always recorded a named argument's alias on the bound child expression, so a
bind callback could read it back with `GetAlias()`. v2.0 added
`FunctionProperties::capture_argument_aliases`, defaulting to **false** — so on
v2.0 those aliases come back **empty**.

Any function that derives named parameters from argument aliases binds every
named argument as unnamed and then fails at run time. The fix is to opt in:

```cpp
func.SetCaptureArgumentAliases(true);
```

If your extension has functions taking `name := value` style arguments, assume
you are affected and check. A green canary build does not clear you of this —
only a test that actually passes a named argument does.

### 8. `StructVector::GetEntries` changed element type

```
v1.5:  vector<unique_ptr<Vector>> &     ->  *entries[i]
v2.0:  vector<Vector> &                 ->   entries[i]
```

`*entries[i]` becomes `no match for operator*`. Mechanical, but it can be many
sites (19 in urlpattern alone). `duckdb_webbed`'s header has a
`CompatStructGetField(Vector &, idx_t)` helper for exactly this.

## Deprecated is not removed — check before you port

Not everything that changed is a hard break, and porting the soft ones costs
effort while buying nothing. On `main` these are `[[deprecated]]` but **still
present**, so they compile with a warning:

```
Vector::ToUnifiedFormat(count, data)     DataChunk::SetCardinality(idx_t)
DataChunk::SetValue(col, idx, val)       Vector::Flatten(count)
Vector::Resize
```

`ConstantVector::GetData<T>(Vector &)` also still has a non-const overload
returning `T*`, so writes through *it* are fine — only `FlatVector::GetData`
went const-only (change 3).

The genuinely removed ones — the compile errors — are `LogicalType::SetAlias`,
`UnaryExecutor`/`BinaryExecutor`/`TernaryExecutor::ExecuteWithNulls`, and the
public fields of change 6.

You can settle any such question in seconds without a CI round and without
cloning: read the header on `raw.githubusercontent.com/duckdb/duckdb/main/...`.
Do that before porting a whole change class on the assumption it is fatal.

## Which header to start from

The fleet's headers are at different generations, and copying the wrong one costs
a full CI round:

| starting point | covers |
|---|---|
| `duckdb_yaml` | the most complete: v1.4 vector/bind changes **and** the scalar-function/expression accessors of change 6 |
| `duckdb_webbed` | v1.4-era changes plus `PreventStructConstantFolding`, `CompatForceMaxLogicalType` |
| `duckdb_markdown` | the v2.0 changes 1-4, verified green against `main` |

None is a superset. Merge, do not replace — the sources in each repo already call
that repo's helpers by name.

## You cannot test this locally

Your submodule is the stable line, so a local build only proves the v1.5 half.
The v2.0 half is verified by CI, and the loop is ~20 minutes.

Keep a canary job that builds against `main` and can be run on demand:

```yaml
duckdb-next-build:
  if: github.event_name == 'workflow_dispatch'
  uses: duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml@main
  with:
    duckdb_version: main
    ci_tools_version: main
```

Then `gh workflow run <workflow>.yml --ref <your-branch>` drives the port.

**Check for an old failed canary run before your first round.** If the repo ever
ran a canary on its default branch, its job log is a complete v2.0 error list you
can have for free, right now, instead of 20 minutes from now:

```
gh run list --workflow MainDistributionPipeline.yml --limit 20 \
   --json databaseId,conclusion,headBranch,createdAt
```

**A green *build* is not a green canary.** The job runs Build and then Test, and
community-extensions' `test_against_latest` runs tests too — so it gates on both.
An extension can compile against v2.0 and still fail behaviourally.

**Read per-step results, not the job rollup**, and make sure the run is actually
`completed` before you read any conclusion — a rollup queried mid-run reports
`success` for jobs that later fail:

```
gh run view <run-id> --json status --jq .status        # must say "completed"
gh run view <run-id> --json jobs \
   --jq '.jobs[] | "=== \(.name) => \(.conclusion)", (.steps[]|"  \(.conclusion) \(.name)")'
```

This matters in both directions. One of our canary rounds was Build-success /
Test-failure on `linux_amd64` while `linux_arm64` was green end to end — reading
the rollup as "the port does not compile" would have sent the next round chasing
an API problem that did not exist.

**A `linux_amd64` Test failure with `linux_arm64` green may not be yours.** We
have seen this shape on two unrelated extensions against DuckDB `main`:

| repo | ported? | amd64 | arm64 |
|---|---|---|---|
| duckdb_markdown | yes | Build ok, Test **fail** (`readme_integration.test`) | green |
| duck_hunt | **no** — plain default branch | Build ok, Test **fail** (`config_parser.test`) | green |

Different tests, different repos, same asymmetry. Because the second extension
has no compat work on it at all, **this shape is demonstrably not something the
porting introduces.** If your canary comes back amd64-Test-red while arm64 is
green end to end, check the arm64 leg first and report the amd64 failure as a
separate open question rather than folding it into the port. If *both* platforms
fail the same test, that is much more likely to be yours.

**Reading the failure needs the REST API, not `gh run view`.** Because the canary
job calls a *reusable* workflow, `gh run view <run> --log-failed` prints nothing
at all — which reads exactly like a job that produced no errors. Get the job id
first, then fetch its log directly:

```
gh run view <run-id> --json jobs \
   --jq '.jobs[] | select(.conclusion=="failure") | "\(.databaseId) \(.name)"'
gh api repos/:owner/:repo/actions/jobs/<job-id>/logs | grep 'error:'
```

**Dispatch after pushing, and do not push again while it runs.** The standard
`concurrency:` group keys on the workflow and ref, so a push run and a dispatch
run on the same branch cancel each other. Two of my canary runs were cancelled
this way before I noticed I was reading a superseded run.

Two notes on that job. Restricting it to `workflow_dispatch` (or pushes to main)
matters: a push and a `pull_request` both fire on the same commit, and a PR's check
rollup includes both — so an unrestricted canary shows red on every PR. And use
`if:` rather than `continue-on-error:`, because a job calling a **reusable
workflow** accepts only a fixed set of keys; adding `continue-on-error` makes the
whole workflow fail to parse, scheduling zero jobs.

## Two local shortcuts

**Syntax-check single TUs instead of building.** While waiting for build capacity
(or CI), you can verify the pinned-v1.5 half of your changed files in about a
minute: pull each file's compile command out of `build/release/compile_commands.json`
and re-run it with `-fsyntax-only`. One process, no link, no DuckDB rebuild.

**Do not run `make format` as a pre-push step.** On several of these repos
clang-format 11.0.1 — the version `duckdb/scripts/format.py` demands — reports
drift on a *pristine* default branch, while CI's format check is green. Running
it mixes unrelated reformatting into your port diff and makes the review
worthless. Check that your branch adds no *new* drift relative to the default
branch instead.

## If you port several repos at once

Two hazards that cost real time here, both of which produce a *false pass*
rather than a failure — which is why they are worth stating.

**A shared-directory log is a trigger, never evidence.** Agents sharing one
scratch directory wrote `build.sh` and `build.log`, and one nohup'd a `build.sh`
that another had overwritten between write and exec. The log came back full of a
different repo's cmake output and ended in a clean `BUILD_EXIT=0` — very nearly
recorded as "builds green locally" for a project that had never compiled at all.

Use such a file to know *when* something finished, but take the pass/fail claim
from something only your repo could have produced: a test file name that exists
nowhere else, a built artifact with your extension's name and a fresh timestamp,
a function name unique to your source. Best of all, let the result come back
through the tool result rather than off disk. (Output files the tool itself
creates for background commands are already collision-safe — they are named per
invocation. The exposure is only scripts you write into a shared directory.)

**Wait on your own PID, not a `pgrep` pattern.** `pgrep -f 'make release'`
matches every concurrent build, not yours. It makes a waiter block until the
*last* one finishes, and — worse — can report "gone" while your own build is
still running. Use `until ! kill -0 $PID 2>/dev/null; do ...; done`.

Also expect memory, not cores, to be the binding constraint: parallel DuckDB
builds OOM long before they run out of CPU, and an OOM-killed `g++` surfaces as
`internal compiler error` / `Killed` / `signal 9`, which reads exactly like a
source bug. Gate on `MemAvailable` and drop to `-j2`.

## Order of work

1. **Look for an old failed canary log first** (see above). If one exists you
   start with the real error list instead of a guess.
2. Grep for every affected site — but know that grep does not find change 6
   (fields that became accessors) or change 7 (argument aliases) at all, so the
   grep is a starting point, never the work list.
3. Pick the right header to start from, merge rather than replace, and check
   your C++ standard before copying `if constexpr` shims.
4. Port one change class at a time.
5. After each, build against the pinned DuckDB and run the full suite. Behaviour
   on the version you actually ship must not move — and for anything touching
   nulls, *measure* old against new rather than reasoning about it. Put the NULL
   inside a non-constant vector or you are not testing the path you changed.
6. Push, then dispatch the canary, then leave the branch alone until it finishes.
7. Read the result on a **completed** run, per step, per platform. Build and Test
   are separate; a green build is not a green canary.
8. Repeat. Expect more errors after the first batch clears — the v2.0 build stops
   at the first failing translation unit, so every error list is a floor, not a
   total.

And one thing the canary cannot tell you: change 7 fails at **run time** with a
green build. If your extension has functions taking `name := value` arguments,
add a test that passes one, or you will ship a break that CI never saw.
