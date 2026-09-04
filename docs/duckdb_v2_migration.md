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

There is no drop-in shim, because the replacement has a different shape: v2.0's
`Execute` adds nulls when the lambda returns `optional<RESULT_TYPE>`.

If your function used `ExecuteWithNulls` only to map NULL to a value rather than
to *produce* nulls, an explicit loop is simpler and version-agnostic:

```cpp
UnifiedVectorFormat vdata;
CompatToUnifiedFormat(input, count, vdata);   // v2.0 dropped the count parameter
const auto in = UnifiedVectorFormat::GetData<string_t>(vdata);
result.SetVectorType(VectorType::FLAT_VECTOR);
auto out = FlatVector::GetData<bool>(result);
for (idx_t i = 0; i < count; i++) {
    const auto idx = vdata.sel->get_index(i);
    out[i] = vdata.validity.RowIsValid(idx) && !in[idx].GetString().empty();
}
```

**Verify the old and new against each other**, because this is the change most
likely to alter behaviour quietly. Comparing ours showed the original lambda's
`RowIsValid` branch was *dead*: DuckDB's default null handling propagates above
the function, so a null input produced NULL either way and the branch never
decided anything.

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

## Order of work

1. Grep for every affected site first — the compiler reports only the first few.
2. Add the compat header; port one change at a time.
3. After each, build against the pinned DuckDB and run the full suite. Behaviour
   on the version you actually ship must not move.
4. Dispatch the canary to check the other half.
5. Repeat. Expect more errors after the first batch clears — the v2.0 build stops
   at the first failing translation unit, so the error list is a floor, not a total.
