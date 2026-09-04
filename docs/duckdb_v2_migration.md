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

## The three changes

### 1. `LogicalType::SetAlias` → `WithAlias`

`WithAlias` returns a copy rather than mutating a type whose type-info may be
shared. Use the `CompatWithAlias` shim above.

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

Grep for **all** of them before starting; the compiler stops at the first few:

```
grep -rn 'vector<string> &names\|vector<string> &return_names' src/
```

### 3. `UnaryExecutor::ExecuteWithNulls` — removed

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
