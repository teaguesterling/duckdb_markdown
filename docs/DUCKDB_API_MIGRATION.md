# DuckDB C++ Internal API: v1.5.x → v1.6.0-dev Migration Guide

> **Audience:** maintainers of DuckDB community extensions that compile
> against the duckdb submodule and build into a `.duckdb_extension`.
> **Status:** field notes from migrating `duckdb_markdown` to build green
> against both `duckdb_version: v1.5.3` (current stable) and
> `duckdb_version: main` (currently `v1.6.0-dev7491`, commit `bbd990d554`,
> on the way to v1.6.0).
>
> If you've seen any of these symptoms in CI and didn't know why —
> this is what's happening.

DuckDB's C++ *internal* API (`src/include/duckdb/...`) has churned non-trivially
between the `v1.5.x` line (Feb–Mar 2026) and the post-`ffdceae563` main branch
(Apr 2026). The **C API** (`duckdb.h`) is stable; the **C++ internal API**
that extensions consume directly is not, and is explicitly versioned only via
the `DUCKDB_EXTENSION_API_VERSION_{MAJOR,MINOR,PATCH}` macros — which cover
the *extension boundary*, not every `#include`d internal header.

That gap is what this guide covers.

**API-equivalence note.** All four breaking changes below landed on `main`
after the last stable tag. Verified by `git show` against:
`v1.4.3`, `v1.5.0`, `v1.5.1`, `v1.5.2`, `v1.5.3`, and `main@bbd990d554`.
The v1.4 and v1.5 lines are *identical* on every API surface this guide
covers — there is no cliff inside that range. So whether you're bumping from
v1.4.4 or from v1.5.3, the migration is the same one this guide describes.

## The four real gaps we hit

### 1. `UnaryExecutor::ExecuteWithNulls<INPUT,RESULT>` — removed in main

**Where:** `duckdb/common/vector_operations/unary_executor.hpp`.

**Cause:** duckdb commit
[`987ea2c409` *"Functions can throw errors"* (#15166)](https://github.com/duckdb/duckdb/commit/987ea2c409)
removed `ExecuteWithNulls`. The (already-existing) `Execute<INPUT, RESULT>`
overload remains: `(input, result, count, fn)` where `fn` takes the input
and returns the result. NULL propagation is handled by the framework; the
lambda only sees defined inputs.

**v1.4.x / v1.5.x:** both `ExecuteWithNulls` (lambda takes `(input, ValidityMask&, idx)`)
and `Execute` (lambda takes `(input)`) exist.

**main:** only `Execute` remains.

**Symptom in CI:**
```
error: 'ExecuteWithNulls' is not a member of 'duckdb::UnaryExecutor'
```

**Migration:** if your old lambda only used the mask to short-circuit on NULL,
just drop the mask args and switch to `Execute`. NULL inputs no longer reach
the lambda — the framework writes NULL to the output automatically.

If your old lambda *needed* the mask to derive a non-NULL output from a NULL
input (e.g. "treat NULL as zero" semantics), you have a small behavior choice:
either accept the new NULL-in/NULL-out (usually more correct) or pre-cast the
input vector to a non-nullable scratch column before calling `Execute`.

**Bonus tip:** if your function can throw, decorate with `FunctionErrors`:
`Execute<...>(in, out, n, fn, FunctionErrors::CAN_THROW_RUNTIME_ERROR)`.

### 2. `Vector::SetValue` + `Value::DefaultCastAs` — the silent-NULL alias trap

**Where:** `duckdb/common/vector/flat_vector.cpp`, `duckdb/common/vector/string_vector.cpp`.

**Cause:** duckdb commit `ffdceae563` *"Move Vector::SetValue to virtual
VectorBuffer::SetValue"* (Apr 9 2026) made `Vector::SetValue(idx, val)` a thin
dispatcher to `buffer->SetValue(type, idx, val)`. The concrete implementations
(`StandardVectorBuffer::SetValue`, `VectorStringBuffer::SetValue`, etc.) all
start with:

```cpp
if (!val.IsNull() && val.type() != type) {
    SetValue(type, index, val.DefaultCastAs(type));
    return;
}
```

`Value::DefaultCastAs(target_type)` uses a stack-local `CastFunctionSet` — it
does **not** see extension-registered casts (`loader.RegisterCastFunction(...)`).
If your column type is an *aliased* VARCHAR (e.g. `MarkdownType` = VARCHAR with
alias `"markdown"`) and your value is a plain `Value(std::string)` (VARCHAR with
no alias), then:

1. `val.type() != type` is true (different aliases)
2. `val.DefaultCastAs(MarkdownType)` returns a **NULL Value** (no built-in cast,
   extension cast invisible)
3. SetValue silently writes NULL with validity cleared
4. Your column is NULL — no exception, no warning

The same trap fires for structural mismatches in `MAP<K,V>`, `STRUCT<...>`,
and `LIST<STRUCT<...>>` when your `Value::MAP` / `Value::STRUCT` / `Value::LIST`
constructor produces a type that's structurally similar but not bitwise-equal
to the column's bind-time type.

**v1.4.x / v1.5.x:** the old `Vector::SetValue` implementation tolerated alias mismatches
silently (copied the data because the InternalType matched). Tests passed.

**main:** new path strict; silent NULL.

**Symptom in CI:**
```
Mismatch on row 1, column ("content" IS NOT NULL)
0 <> true
```
where you set `content` to a non-empty string and the column type is
`MarkdownType` (or any aliased VARCHAR), or:
```
Mismatch on row 1, column len(wikilinks)
1 <> 5
```
where you set a LIST with 5 STRUCT entries but `len(...)` returns 1.

**Migration — the idiomatic fix:**

```cpp
// Instead of:
output.data[col].SetValue(idx, val);

// Use:
output.data[col].SetValue(idx, val.CastAs(context, output.data[col].GetType()));
```

`Value::CastAs(ClientContext&, target_type)` uses the **catalog's** cast set,
which DOES include your extension-registered casts. Also handles structural
mismatches in complex types. Both `v1.4.x` / `v1.5.x` and `main` have this signature.

Wrap it in a small helper so the intent is clear:

```cpp
static inline void SetValueCasted(ClientContext &context, Vector &vec, idx_t idx, const Value &val) {
    vec.SetValue(idx, val.CastAs(context, vec.GetType()));
}
```

Apply at every site that passes a typed `Value` (`Value(std::string)` to an
aliased VARCHAR column, `Value::MAP(...)`, `Value::STRUCT(...)`,
`Value::LIST(...)`). Primitive `Value::BIGINT` / `Value::INTEGER` /
`Value::BOOLEAN` to matching numeric columns don't need it (val.type() ==
column.type() exactly).

**Don't bother with `Vector::Append(Value)`** for this — it calls
`VectorBuffer::AppendValue` which calls `SetValue` internally, so the trap
still fires.

**Don't reach for direct `FlatVector::GetData<T>` writes** as a workaround —
they have their own portability landmine; see §4.

### 3. `DataChunk::SetCardinality(N)` no longer propagates to vectors

**Where:** `duckdb/common/types/data_chunk.cpp`.

**Cause:** duckdb's vector-buffer refactor introduced per-vector
`VectorBuffer::v_size` tracking. Query operators (`VariadicExecutor`,
some others) now consult each vector's `Size()` *in addition to* the chunk's
`count`. `DataChunk::SetCardinality(N)` only updates `chunk.count`;
**individual vectors stay at `Size() == 0`** even after you've populated their
data via `SetValue`.

A new sibling `DataChunk::SetChildCardinality(N)` was added that calls
`FlatVector::SetSize(v, N)` on every child vector. `v1.4.x` / `v1.5.x` has neither
`SetChildCardinality` nor `FlatVector::SetSize`.

**v1.4.x / v1.5.x:** `SetCardinality` is the only API; vectors don't track size
individually; query operators read `chunk.count` only. Works.

**main:** `SetCardinality` does not propagate; you must call
`SetChildCardinality` (or per-vector `FlatVector::SetSize`).

**Symptom in CI:**
```
INTERNAL Error: Mismatch in input vector sizes for VariadicExecutor —
expected 0 rows but got 5
```
on a table function whose output looks fine until a downstream scalar
expression touches it. (You write rows via `SetValue` in a loop, increment
your `output_idx`, and call `output.SetCardinality(output_idx)` at the end.)

This bug is *invisible* until you've also fixed the SetValue alias trap (§2):
with NULL content, downstream operators short-circuit on NULL and never read
the underlying vector data, so the size mismatch never surfaces. Once content
is populated, the next operator that needs vector data trips the check.

**Migration — cross-version `SetCardinalityPropagating`:**

Add `src/include/duckdb_compat.hpp`:

```cpp
#pragma once
#include "duckdb/common/types/data_chunk.hpp"
#include <type_traits>
#include <utility>

namespace duckdb {
namespace your_ext_compat {

template <typename T, typename = void>
struct has_set_child_cardinality : std::false_type {};
template <typename T>
struct has_set_child_cardinality<T, std::void_t<decltype(std::declval<T &>().SetChildCardinality(idx_t(0)))>>
    : std::true_type {};

template <typename Chunk>
inline void SetCardinalityPropagating(Chunk &chunk, idx_t count) {
    if constexpr (has_set_child_cardinality<Chunk>::value) {
        chunk.SetChildCardinality(count);
    } else {
        chunk.SetCardinality(count);
    }
}

}  // namespace your_ext_compat
}  // namespace duckdb
```

Then everywhere you used to write `output.SetCardinality(N)`, use
`your_ext_compat::SetCardinalityPropagating(output, N)`. SFINAE picks the right
call at compile time, so the same source compiles against both `v1.5.x` and
`main` with no `#if` ladders.

### 4. `FlatVector::GetData<T>` and `Validity` split into const + Mutable

**Where:** `duckdb/common/vector/flat_vector.hpp`.

**Cause:** main's accessors now return `const T*` / `const ValidityMask&` to
enforce write-discipline. New `GetDataMutable<T>` / `ValidityMutable` overloads
exist for writes.

**v1.4.x / v1.5.x:** unified accessors return non-const (`T *GetData<T>(Vector&)`,
`ValidityMask &Validity(Vector&)`).

**main:** const-returning by default; need `GetDataMutable<T>` and
`ValidityMutable` for writes.

**Symptom in CI:**
```
error: passing 'const duckdb::string_t' as 'this' argument discards qualifiers
error: passing 'const duckdb::ValidityMask' as 'this' argument discards qualifiers
```

**Migration:** prefer `SetValueCasted` (§2) — you almost never need to write
flat-vector buffers directly from a table function once the alias-cast trap
is fixed. If you genuinely need direct writes, use a `const_cast`:

```cpp
auto *data = const_cast<string_t *>(FlatVector::GetData<string_t>(vec));
data[idx] = StringVector::AddString(vec, str);
```

This is a no-op on `v1.4.x` / `v1.5.x` (whose `GetData<T>(Vector&)` already returns
non-const) and a stripping cast on `main`. It's safe because the underlying
vector is non-const — we just happen to access its data through a const
overload — but it's an unmistakable code smell, so keep the use narrow.

For validity, the same pattern: `const_cast<ValidityMask &>(FlatVector::Validity(vec)).SetValid(idx)`.

## Upgrade checklist (for other extensions)

When you bump your duckdb submodule from v1.4.x / v1.5.x to a current `main`
pin, work through these in order:

- [ ] **Compile errors first.** Grep your source for `UnaryExecutor::ExecuteWithNulls` —
      replace with `UnaryExecutor::Execute<INPUT, RESULT>`, drop the
      `ValidityMask&` / `idx_t` from the lambda, accept NULL-in / NULL-out
      (or pre-cast the input column).
- [ ] **Find every `SetValue` call that passes a typed `Value` to a non-primitive
      column** (aliased VARCHAR, MAP, STRUCT, LIST). Replace with a helper that
      calls `val.CastAs(context, vec.GetType())` before SetValue. Don't migrate
      to `Vector::Append(Value)` thinking it sidesteps the trap — it doesn't.
- [ ] **Find every `DataChunk::SetCardinality` call in a table-function execute.**
      Replace with a SFINAE-dispatched helper that calls `SetChildCardinality`
      on duckdb main and `SetCardinality` on v1.4.x / v1.5.x. See §3.
- [ ] **If you have direct `FlatVector::GetData<T>` writes, audit them.** If the
      cast site can be migrated to `SetValueCasted`, prefer that. Otherwise
      wrap the write in a `const_cast` (no-op on v1.4.x / v1.5.x, required on main).
- [ ] **Run `make format-check` against your bumped submodule.** The
      `.clang-format` config tightened slightly; `make format-fix` is the
      easiest catch-up.
- [ ] **Verify both matrix entries.** If your `MainDistributionPipeline.yml`
      builds against `main` *and* a stable tag, ensure both go green. If only
      one passes, the SFINAE detection is in the wrong direction — re-read §3.

## What we didn't fix in this PR (deferred items)

- The duckdb-extension community-pipeline `Windows (windows_amd64, ...)` job
  has been failing since ~April 27 with `error: pathspec '...\vcpkg' did not
  match any file(s) known to git`. This is an upstream `extension-ci-tools`
  / `lukka/run-vcpkg@v11.1` regression, not anything in the extension itself.
  Affects everyone equally; we tracked but did not try to fix.
- We did not migrate to `Vector::Append(Value)`. The `4bbfea6f6e
  "Prefer using \`Vector::Append(Value)\` instead of \`DataChunk::SetValue()\`"`
  commits in main mark `DataChunk::SetValue` as the deprecated form, not
  `Vector::SetValue`. Our SetValueCasted helper still uses `Vector::SetValue`,
  which is not deprecated. If duckdb's intent is to deprecate that too, we'll
  need to revisit, but the bookkeeping (per-row index increments vs append
  ordering) is non-trivial to refactor.

## See also

- duckdb main HEAD at the time of writing: `bbd990d554` (`v1.6.0-dev7491`)
- The four pivotal commits in chronological order:
  - `987ea2c409` "Functions can throw errors" (#15166) — removed `ExecuteWithNulls`
  - `ffdceae563` "Move Vector::SetValue to virtual VectorBuffer::SetValue"
  - `b5ed9c59bf` "Move vector function implementations into VectorBuffer" (#22030)
  - `52611d20b9` "Deprecate DataChunk::SetValue, switch to using .Append(Value)"
- `4bbfea6f6e` "Prefer using `Vector::Append(Value)`..." (#22223) — example
  migrations across the duckdb tree (mostly `DataChunk::SetValue` callsites in
  extensions like `autocomplete`, `icu`, `excel`).

Found something else that broke? PRs to this doc welcome. The whole point is
that the *next* extension maintainer doesn't have to re-derive any of this.
