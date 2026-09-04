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

**And probe the thing itself, never a proxy for it.** The same mistake one level
up nearly shipped fleet-wide. `CompatName` was selected by
`__has_include("duckdb/common/identifier.hpp")` — the header's existence standing
in for "bind signatures take `Identifier`". Those are two different facts, and
they have **already come apart upstream**:

```
v1.5-variegata @ b155d6f63c (the pin)   no identifier.hpp    bind: vector<string>
v1.5-variegata @ branch TIP             HAS identifier.hpp   bind: vector<string>   <-- proxy fails here
main (v2.0)                             HAS identifier.hpp   bind: vector<Identifier>
```

`identifier.hpp` was **backported to the stable branch** without changing
`table_function_bind_t`. The probe is right today only because the pin predates
the backport; on the next submodule bump it flips `CompatName` to `Identifier` on
a DuckDB that still wants strings, and every bind signature in the extension
stops compiling at once. Since v2.0 deprecates rather than deletes and backports
freely, *the header arrives before the behaviour does* — so a header probe is a
leading indicator, not a test.

Ask DuckDB what its own type is instead (see change 2).

**The same hazard sits on `__has_include("duckdb/common/vector/list_vector.hpp")`**,
which several headers use to gate `CompatSetOutputCardinality`. That one fails
*loudly* — a backported header without `SetChildCardinality` is a compile error,
not a wrong branch — but it still breaks the **shipped** build on a submodule
bump. Probe the member (`SetChildCardinality`) and let the `__has_include` do
nothing but pull the header in.

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

Sorted by how they announce themselves. **The two that announce themselves not at
all are the ones to plan around** — nothing in your toolchain will find them, and
a fully green canary does not clear you of either.

| # | change | how you find out |
|---|---|---|
| 1 | `LogicalType::SetAlias` removed → `WithAlias` | compile error |
| 2 | bind signatures take `vector<Identifier>` | compile error |
| 3 | `FlatVector::GetData<T>` is read-only | compile error |
| 4 | `ExecuteWithNulls` removed | compile error |
| 5 | `FunctionSet<T>::functions` is `shared_ptr<const T>` | compile error |
| 6 | public fields → accessors (`varargs`, `return_type`, …) | compile error, **no grep finds it** |
| 7 | `capture_argument_aliases` defaults to false | **RUNTIME, green build** |
| 8 | `StructVector::GetEntries` element type | compile error |
| 9 | per-vector accessor headers moved | compile error (reads as a missing symbol) |
| 10 | `Vector::Reference` gained `count_t` | compile error — **must be `#ifdef`, not `if constexpr`** |
| 11 | `ScalarFunction` ctor dropped a positional parameter | **may still compile, silently wrong** |
| 12 | `DefaultMacro` changed shape | compile error |
| 13 | throwing scalar functions must `SetFallible()` | **RUNTIME, green build, one arch only** |
| 14 | `FlatVector::Validity` const split | compile error, **at the mutation, not the call** |
| 15 | filter pushdown reworked | compile errors — **plus silent wrong results** |
| 16 | `CreateInfo`/`DropInfo` names → private `QualifiedName` | compile error (storage extensions only) |
| 17 | result / prepared-statement names are `Identifier` | compile error |
| 18 | `SetCardinality` → `SetChildCardinality` | **compiles; silently overwrites data** |

Classes 7 and 13 break at run time on a build that compiles cleanly everywhere.
Class 11 can compile and misbehave. Class 15 can return **wrong rows** with no
error at all, if your pushdown *is* the filter rather than an optimisation, and
class 18 can silently **overwrite column data** if shimmed uniformly.
Class 13 additionally shows up on **one CI architecture only**, because its
enforcement is an assertion — so "green on arm64" is not evidence of anything.

Nothing in this list is found by the greps in change 2. Classes 6, 7 and 13 are
not found by any grep of the DuckDB API at all — for those you grep your *own*
code, for field accesses, alias reads, and `throw`.

### 1. `LogicalType::SetAlias` → `WithAlias`

`WithAlias` returns a copy rather than mutating a type whose type-info may be
shared. Use the `CompatWithAlias` shim above.

**Make the entry point a concrete function, not a deduced template.** This one
bites on the pinned v1.5 build, not on v2.0:

```cpp
template <class TYPE = LogicalType>          // WRONG -- the default is inert
LogicalType CompatWithAlias(TYPE type, string alias);
```

Deduction wins over the default, so `CompatWithAlias(LogicalType::VARCHAR, "yaml")`
deduces `TYPE = LogicalTypeId` — `LogicalType::VARCHAR` is a `static constexpr
LogicalTypeId` (`types.hpp`), not a `LogicalType` — and hard-errors inside the
shim with *"request for member 'SetAlias' in 'type', which is of non-class type
'duckdb::LogicalTypeId'"*. Take a concrete `LogicalType` parameter so the
implicit `LogicalTypeId` conversion happens at the call site, and keep only the
tag-dispatch `Impl` overloads templated. Code that already wrote
`LogicalType(LogicalTypeId::VARCHAR)` never sees this, which is exactly how it
survives review.

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
signatures move**, plus the few places a *runtime* string crosses the boundary.

Define the name type by **asking DuckDB for it**, not by probing a header (see
the warning above — the header probe is already wrong on the stable branch tip):

```cpp
#include "duckdb/function/table_function.hpp"

// TableFunctionBindInput::input_table_names has the same element type as the
// bind out-parameter on both lines: table_function.hpp:110/288 on the pin,
// :123/319 on main. This cannot drift, because it IS the thing that changed.
using CompatName = typename std::remove_reference<decltype(
    std::declval<TableFunctionBindInput &>().input_table_names)>::type::value_type;

inline string CompatNameStr(const string &name) { return name; }
#ifdef DUCKDB_HAS_IDENTIFIER
// Declares the overload only. It must NOT decide CompatName. Both overloads
// coexist fine when CompatName is still string.
inline string CompatNameStr(const Identifier &id) { return id.GetIdentifierName(); }
#endif

inline CompatName CompatMakeName(string name) { return CompatName(std::move(name)); }
```

**Write it WITHOUT `typename` at namespace scope** — but for the right reason,
which is simply that it buys nothing there. An earlier draft of this guide said
`typename` outside a template is "C++20-only" and breaks MSVC. **That is wrong**,
and it was propagated widely before anyone checked it. CWG 382 permitted
`typename` on non-dependent qualified names from C++11 on, and
`g++ -pedantic-errors` accepts it at `-std=c++11`, `c++14` and `c++17`:

```cpp
using T = typename std::remove_reference<std::vector<int> &>::type::value_type;  // fine
```

(Whether some MSVC version rejects it is a claim nobody in this effort actually
tested; treat it as unverified.) Drop the keyword because it is redundant, not
because it is illegal — and note that **inside** the template it is genuinely
required, since `D` is dependent there:

```cpp
template <class R, class A, class B, class C, class D>
struct CompatBindNamesOf<R (*)(A, B, C, D)> {
	using type = typename std::remove_reference<D>::type::value_type;   // REQUIRED
};
using CompatName = CompatBindNamesOf<table_function_bind_t>::type;      // no typename
```

Getting the reason wrong matters more than the keyword: someone who believes
`typename` is illegal outside C++20 will delete it from a context where it is
mandatory.

**Assert the coupling.** Deriving the type fixes one failure mode and leaves
another: `CompatName` can resolve to `Identifier` on a DuckDB whose
`identifier.hpp` this header did not find, so the `Identifier` overload was never
declared. This holds on both lines and is not a tautology:

```cpp
static_assert(std::is_same<decltype(CompatNameStr(std::declval<const CompatName &>())), string>::value,
              "CompatNameStr must accept the derived CompatName on every DuckDB line");
```

(When it does fire, the failure is a hard error inside the `decltype` —
*"'CompatNameStr' was not declared in this scope"* — so the message string never
prints. Right line, right file, but the comment beside it does the explaining.)
Do **not** assert `is_same<CompatName, string>`: true on the pin, false on main,
so it hard-fails the v2.0 build. And asserting `CompatName` equals the expression
that *defines* it can never fire.

#### The same bomb, a second time: `child_list_t` keys

STRUCT/UNION field names are the other boundary that moved, and a
`CompatMakeIdentifier`-style helper selected by `__has_include` has **exactly the
same defect**. Verified: `child_list_t`'s key is `std::string` on the pin **and on
the stable branch tip**, and `Identifier` only on main — so a header probe flips
the constructed key type while `child_list_t` still wants strings.

Derive it from the same boundary:

```cpp
using CompatIdentifierKey = child_list_t<LogicalType>::value_type::first_type;
```

Only **construction** has to choose; *reading* a key can be overloaded
unconditionally. String **literals** are fine either way — `Identifier` is
implicitly constructible from `const char *` — so this only bites where a
**runtime** string becomes a field name.

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
purpose.

**`const_cast<T *>(FlatVector::GetData<T>(v))` is not a fix — it is a silent
data-corruption path.** The two accessors do different work, not just different
constness:

```
GetData         -> vector.GetBufferRef()->GetData()      // no un-share
GetDataMutable  -> vector.BufferMutable().GetData()      // un-shares COW buffer first
```

Casting the const away compiles and then writes into a buffer that may still be
shared with another vector. If a repo "already handled" the const change that
way, it has a latent bug rather than a working shim:

```
grep -rn 'const_cast<[^>]*\*>(FlatVector::GetData' src/
```

Probe for the member and route the write path:

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

For **read-only** introspection no probe is needed at all: `shared_ptr<const T>`
names a valid type on both versions, so two overloads and partial ordering settle
it.

```cpp
template <class T> inline const T &CompatFunctionRef(const T &f)                   { return f; }
template <class T> inline const T &CompatFunctionRef(const shared_ptr<const T> &f) { return *f; }
```

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

Most of these need **no shim at all** — the setters already exist on the pin
(`SetNullHandling` at `function.hpp:199`, likewise `SetReturnType`,
`GetReturnType`, `GetStability`, and `BaseExpression::GetAlias`). `varargs` is
the one member that genuinely needs a branch, because v1.5's `SimpleFunction` has
a public field and no `SetVarArgs`.

The grep that finds this class is on the **assignment**, not the setter —
searching for `SetStability|SetNullHandling|SetVarArgs` only finds code that has
already been fixed:

```
grep -rnE '\.(null_handling|stability|varargs|bind|serialize) *=' src/
```

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

DuckDB does this to itself: `src/function/scalar/struct/struct_pack.cpp` calls
`SetCaptureArgumentAliases(true)` with a comment explaining why. That also means
an extension emitting a `struct_pack` with aliased children is relying on
*upstream's* opt-in rather than its own.

**COPY option keys are a different surface — do not confuse the two.** A COPY
bind reading `input.info.options` (a map) is untouched by this change; only
*argument aliases* on bound child expressions are. The two look similar and are
unrelated, so a repo with COPY options and no alias reads is not affected here.
The grep that settles it is `GetAlias` / `capture_argument_aliases`, not
`option`.

If your extension has functions taking `name := value` style arguments, assume
you are affected and check.

**The dangerous case is named arguments that are *untested*.** A repo whose suite
exercises the named-argument path gets caught by the canary's Test step. A repo
where named arguments exist but nothing tests them ships a green build and a
broken function. Check which of the two you are before trusting a green canary. A green canary build does not clear you of this —
only a test that actually passes a named argument does.

### 8. `StructVector::GetEntries` changed element type

```
v1.5:  vector<unique_ptr<Vector>> &     ->  *entries[i]  /  entries[i]->SetValue(...)
v2.0:  vector<Vector> &                 ->   entries[i]  /  entries[i].SetValue(...)
```

The *element type* changed, not just the header location, so `*entries[i]` and
`entries[i]->` both stop compiling. Mechanical, but it can be many sites (19 in
urlpattern alone). `duckdb_webbed`'s header has a
`CompatStructGetField(Vector &, idx_t)` helper for exactly this.

### 9. The per-vector accessor headers moved

`FlatVector`, `ListVector`, `StructVector` and friends split out of
`duckdb/common/types/vector.hpp` into one header each under
`duckdb/common/vector/`, and **`duckdb.hpp` no longer pulls them in
transitively**. It presents as `'StructVector' has not been declared`, which
reads like a missing symbol rather than a moved header.

There are **six** of them, and it is easy to include three and be caught later by
the fourth — `ArrayVector` in particular tends to surface only deep in a file:

```
duckdb/common/vector/{flat,list,struct,array,constant,dictionary}_vector.hpp
```

Include them defensively — including in your compat header, which names
`FlatVector` at namespace scope:

```cpp
#if __has_include("duckdb/common/vector/flat_vector.hpp")
#include "duckdb/common/vector/flat_vector.hpp"
#endif   // and the other five
```

### 10. `Vector::Reference(const Value &)` gained a count — and cannot be `if constexpr`

```
v1.5:  Reference(const Value &)
v2.0:  Reference(const Value &, count_t)      // pass args.size()
```

`count_t` is a strong type (`duckdb/common/types/size.hpp`) whose constructor
from `idx_t` is **explicit**, and the type does not exist on v1.5 at all.

**That last part forces `#ifdef`, not `if constexpr`.** `if constexpr` discards
the untaken branch but the compiler still *parses* it, and a missing
non-dependent name is a hard error at parse time. So this one must be gated on
`__has_include("duckdb/common/types/size.hpp")`. It is the one change in this
document where the tag-dispatch/`if constexpr` idiom does not work.

### 11. `ScalarFunction`'s constructor dropped a positional parameter

`bind_scalar_function_extended_t` was removed from between `bind` and
`statistics`. Any call site threading a value through a run of `nullptr`s to
reach the positional tail now **shifts one slot** — landing a `nullptr` on the
`LogicalType varargs` parameter. It may still compile, which is what makes it
dangerous.

Stop counting positions: set it afterwards with `SetStability()`, which exists
on both versions. (Stability is also no longer a public field; it moved into a
protected `FunctionProperties`.)

### 12. `DefaultMacro` changed shape

```
v1.5:  {schema, name, parameters[8], named_parameters[8], macro}
v2.0:  {schema, name, macro_definition}
```

The signature moved *inside* the definition string —
`"(a, b, c := 'x') AS body"` — parsed as `CREATE MACRO __dummy__...`.
`DefaultTableMacro` did **not** change, so a repo can be hit by one and not the
other.

### 13. Fallible scalar functions must say so — a SILENT runtime break

Like change 7, a **runtime contract with no compile error and nothing in the
DuckDB API to grep for**. v2.0 requires a scalar function that can throw during
execution to declare it. Throwing from one that has not is an
`InternalException`:

```
INTERNAL Error: Scalar function "to_xml" threw an execution error, but the
function is not marked as fallible - the function must call SetFallible().
```

**Read this before the error message above: v1.5 already has this flag.**
`BaseScalarFunction::SetFallible()` is at `function.hpp:211` on the pin, and
`FunctionErrors errors` (defaulting to `CANNOT_ERROR`) already feeds
`Expression::CanThrow()`. **v2.0 did not add the contract — it added
enforcement of a contract that already existed and that these extensions have
been quietly violating on the version they ship.** That reframes the whole
change: it is not a v2.0 compat chore, it is a latent correctness bug on v1.5
that v2.0 surfaces.

**A plain function needs no shim; a `FunctionSet` does.** This is the trap, and
most functions live in sets:

```
pin:   FunctionSet has `vector<T> functions`         and NO set-level SetFallible
main:  FunctionSet has SetFallible()                 and `vector<shared_ptr<const T>>`
```

So neither spelling works on both. Probe for the set-level member:

```cpp
// true_type  -> set.SetFallible();                              // v2.0 set, or any plain function
// false_type -> for (auto &f : set.functions) f.SetFallible();  // v1.5 set, members still mutable
```

A guide that just says "call it directly" leaves **every `ScalarFunctionSet`
unmarked on the shipping version** — in one repo that was 10 sets against 8 plain
functions, i.e. most of the surface.

**It is not a no-op on the version you ship — it is unenforced.** The
distinction matters: v1.5 *has* the flag and *consults* it; it simply does not
enforce the contract. Calling it a no-op invites blanket-marking on the theory
that the pinned version cannot care, and it can. On v1.5 `errors` feeds
`BoundFunctionExpression::CanThrow()`, which gates conjunct reordering
(`expression_heuristics`, `adaptive_filter`), filter pushdown
(`pushdown_get` / `_projection` / `_outer_join`) and dictionary-expression
caching (`execute_function.cpp`). Declaring a function fallible makes the
shipped planner **strictly more conservative** around it. That direction is safe
— it can only stop a throwing function being evaluated on rows it should not
have been — but it is a real change, so measure it rather than assuming.

**Marking is often a latent v1.5 *fix*, not a cost.** `CanThrow()` is read in
six places on the pin (`expression_heuristics:48`, `pushdown_outer_join:125`,
`pushdown_projection:54`, `pushdown_get:119`, `adaptive_filter:15`,
`execute_function:11`), and every one asks whether a *filter expression* can
throw. An extension that leaves a file-opening function marked
`CANNOT_ERROR` is telling the planner it may hoist that call onto rows a guard
was there to exclude — so `CASE WHEN ok THEN risky(f) END` can be evaluated on
the rows the `CASE` exists to protect. Declaring it fallible restrains exactly
that, which is the point rather than the price.

It also means the interaction is narrower than it first looks: if your own
pushed-down filters are plain column comparisons that never call your functions,
marking them cannot restrain your own pushdown — the two never meet. One port
measured precisely this and its pushdown tests were unchanged.

**Mark precisely, not defensively.** The property is optimizer-visible, so
over-marking is a small real pessimisation of the shipped binary, not a free
hedge.

A one-step `grep throw` **over-marks**. Make it two steps: grep `throw`, then
check whether an enclosing handler *swallows* it. The tell is a catch-all that
**returns** rather than rethrows:

```cpp
catch (...) { return false; }              // NOT fallible -- the throw never escapes
catch (const InvalidInputException &) { throw; }   // IS fallible
```

Two functions can look identical until you read the handler. In one repo, 18 of
21 scalar functions were fallible and the 3 that were not could not be told apart
by grepping `throw` at all. Also follow the transitive callers of any throwing
*helper* — and note that including a throwing helper's header proves nothing;
one repo had three files including such a header for an unrelated parser and
never reaching the throw.

**Registration shape can block the fix.** `loader.RegisterFunction(ScalarFunction(...))`
constructs a temporary, so there is no object to call `SetFallible()` on. Hoist
it to a local (or a small `register_fallible` lambda). This compounds with change
5: set members are no longer mutable, so the property must be set **before**
hand-off to a `FunctionSet`.

**Assert the coupling, but assert something that can fail.** A useful guard ties
the derived name type to its overload set — it holds on both lines, and catches
`CompatName` resolving to `Identifier` on a DuckDB whose `identifier.hpp` the
header did not find (so the `Identifier` overload was never declared):

```cpp
static_assert(std::is_same<decltype(CompatNameStr(std::declval<const CompatName &>())), string>::value,
              "CompatNameStr must accept the derived CompatName on every DuckDB line");
```

Do **not** assert `is_same<CompatName, string>` — that is true on the pin and
false on main, so it hard-fails the v2.0 build. And asserting `CompatName` equals
the expression it is *defined* by is a tautology that can never fire.

**Scope notes.** Table functions, COPY and casts are outside this contract —
casts run through `BoundCastInfo`, not `BaseScalarFunction::Execute`. And
`PragmaFunction` derives from `SimpleNamedParameterFunction`, not
`BaseScalarFunction`, so pragmas are unaffected.

**When measuring, make sure your bad input actually throws.** One port's first
probe used a malformed-looking URL that the parser happily accepted, so the probe
was vacuous and measured nothing. Test that your input errors before you trust a
before/after comparison built on it.

**This is the cause of the amd64/arm64 asymmetry described below.** The contract
is violated on *both* arches; only the assertion-enabled image reports it. So the
same commit is green on one leg and red on the other, and the failing test is
always one that exercises an *error path*. Confirmed on two unrelated extensions,
one of which has **no compat work at all** — which is the point: you do not have
to port anything to violate a contract that is new.

```
duckdb_webbed  3 of 4 amd64 failures: to_xml, xml_extract_text, xml_wrap_fragment
duck_hunt      amd64 failure: duck_hunt_load_parser_config   (unported)
```

### 14. `FlatVector::Validity` got the same const split — and hides better

```
v1.5:  Validity(Vector &)              -> ValidityMask &
v2.0:  Validity(const Vector &)        -> const ValidityMask &
       ValidityMutable(Vector &)       -> ValidityMask &
```

Same copy-on-write distinction as change 3 (`ValidityMutable` un-shares through
`BufferMutable()`; `Validity` does not).

**Harder to find than change 3**, because the accessor call still compiles:
`auto &m = FlatVector::Validity(v)` just deduces a const reference. The error
appears later, at the mutation:

```
error: passing 'const duckdb::ValidityMask' as 'this' argument discards qualifiers
```

which names neither `Validity` nor `FlatVector`. So grep for the **mutation**,
then walk back to where the reference was bound:

```
grep -rn 'SetInvalid\|SetValid(\|SetAllInvalid\|SetAllValid' src/
```

### 15. Filter pushdown was reworked — and can go silently wrong

Only bites extensions that implement pushdown, but it bites hard. Four compile
breaks and one semantic landmine.

- `TableFilterSet` moved to `duckdb/planner/table_filter_set.hpp` (a good
  `__has_include` probe — it does not exist on v1.5), made `filters` **private**,
  rekeyed it from `idx_t` to `ProjectionIndex`, and replaced map access with
  `begin()`/`end()` yielding entries with `GetIndex()`/`Filter()`. Presents as
  *"invalid use of incomplete type 'class duckdb::TableFilterSet'"*, which reads
  like a missing include and is actually a moved-and-closed member. Note v1.5's
  `TableFilterSet` has **no** `HasFilters()` — the one you may remember belongs
  to `DynamicTableFilterSet` in the same header.
- `ConstantFilter` → `LegacyConstantFilter`, `InFilter` → `LegacyInFilter`, and
  the enumerators gained a `LEGACY_` prefix, because v2.0 represents filters as a
  general `ExpressionFilter`. There is no SFINAE probe for a type *name*, so gate
  these on the header probe and say so. Take the kind constant from the aliased
  class (`CompatConstantFilter::TYPE`) rather than spelling the enumerator, so
  the name lives in one place.
- `BoundConjunctionExpression::children` is private;
  `GetChildren()`/`GetChildrenMutable()` replace it. Probe the **mutable** one.
- `ClientContext::interrupted` (a public atomic) is gone. `IsInterrupted()`
  exists on **both** versions, so no shim — just use the accessor. The compiler's
  *"did you mean 'Interrupt'?"* points at the wrong member.

**The landmine.** v2.0 keeps the legacy filter classes only for
*deserialization*. A **running** scan is handed an `ExpressionFilter`, so code
matching on the legacy kinds simply never matches. Whether that is harmless
depends entirely on what your pushdown is *for*:

- pushdown as an **optimisation** (best-effort narrowing, every filter still
  re-applied above) → you read more data and return the same rows;
- pushdown as **the filter itself** → **silent wrong results**.

A purely mechanical port cannot tell these apart. Check which shape you have.

Related: v2.0's `TableFilterSet` has a **second** collection for multi-column
filters that iterating the per-column entries does not visit. Since DuckDB
deletes a fully-pushed filter from the plan, an unseen filter is an *unapplied*
filter. There is no correct "ignore it" — detect and refuse
(`NotImplementedException`). Detection is a direct call, not an inference:
`HasMultiColumnFilters()` and `GetMultiColumnFilters()` are members
(`table_filter_set.hpp:25,34`).

The same header also exposes `HasFilter`, `GetFilterByColumnIndex`,
`TryGetFilterByColumnIndex` and their `Mutable` variants — usually cleaner than
iterating, and `TryGet...` returns `optional_ptr`, which removes a lookup-then-
index round trip.

### 16. `CreateInfo`/`DropInfo` names became a private `QualifiedName`

Hits any extension with its own `Catalog`/`SchemaCatalogEntry` — i.e. a storage
extension with an ATTACH backend. `info.schema` and `info.name` are gone;
`GetQualifiedName().Schema()` / `.Name()` (`create_info.hpp:56`),
`GetQualifiedNameMutable()` (`:59`), `CreateSchemaInfo::SchemaName()` and
`CreateInfo::SetSchema(Identifier)` (`:74`) replace them.

Two traps: probe on `GetQualifiedName` (present on v2.0 *and* the stable branch
tip) rather than on the absent field; and the tip's backported
`GetQualifiedName()` returns **by value** while v2.0 returns by reference, so
never bind a reference to `.Name()`.

### 17. Result and prepared-statement names are `Identifier`

`BaseQueryResult::names` and `types` are **private** on v2.0
(`query_result.hpp:55` onward), not merely retyped — so `result.names[col]` and
`result.types[i]` stop compiling outright, whatever you do about the element
type. Use the public `GetNames()` (`:43`, returning `const vector<Identifier> &`)
and `GetTypes()` (`:41`). Result formatters and DDL builders are the usual
victims, and a repo that runs its own internal queries hits this far from any
bind signature.

`PreparedStatement::named_param_map` is **still a public member** — it did not
move behind an accessor, its *element type* changed, from
`case_insensitive_map_t<idx_t>` to `identifier_map_t<idx_t>`
(`prepared_statement.hpp:36`). So field access still compiles and only key-type
assumptions break; there is a `GetNamedParameterMap()` (`:81`) but switching to
it is not required and is easy to over-port. `Execute` and `PendingQuery` take
`identifier_map_t<BoundParameterData>`, and `Appender`'s table-name parameter is
an `Identifier`.

**Probe trap:** do *not* probe this by trying to call `Execute` with an
`identifier_map_t`. `PreparedStatement` has a variadic
`template <class... ARGS> Execute(ARGS...)` that swallows anything, so the probe
answers yes on **both** versions and then fails deep inside the instantiation.
Probe the `GetNamedParameterMap` accessor instead.

### 18. `SetCardinality` → `SetChildCardinality` is NOT a rename — it can overwrite your data

The most dangerous shim in this ecosystem, because it looks like the safest one.
A `CompatSetOutputCardinality` that forwards to `SetChildCardinality` on v2.0 is
**exactly** the substitution upstream warns against, in its own words
(`data_chunk.hpp:72-74`):

> this only sets the chunk's cardinality, it does **not** resize the child
> vectors... Callers that mutate the child vectors directly (e.g.
> `Vector::Append`/`SetValue`) and then call `SetCardinality` rely on this —
> **forwarding to `SetChildCardinality` would resize/overwrite their data**.

So the correct mapping depends entirely on the **order** at the call site:

| your order | v1.5 | correct on v2.0 |
|---|---|---|
| set cardinality, *then* write children | `SetCardinality` | `SetChildCardinality` |
| write children (`SetValue`/`Append`), *then* set cardinality | `SetCardinality` | **`SetCardinalityUnsafe`** |

`SetCardinality` is `[[deprecated]]` but **preserved**, and it forwards to
`SetCardinalityUnsafe` — so the old behaviour is still reachable under a new
name. A single blanket shim cannot be right for both orders.

This is a **silent** corruption: it compiles, and it only manifests as wrong
column data on v2.0. Audit every call site for its order rather than shimming
them uniformly:

```
grep -rn -B8 'CompatSetOutputCardinality\|SetCardinality' src/ | grep -n 'SetValue\|Append'
```

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

Also `[[deprecated]]` but present, and noisy rather than fatal: the **templated**
`Catalog::GetEntry<T>(context, schema, name, if_not_found)` — prefer the
non-template `GetEntry(context, CatalogType, Identifier, Identifier, OnEntryNotFound)`,
which exists on both — and all three of `KeywordHelper::WriteQuoted` /
`WriteOptionallyQuoted` / `EscapeQuotes`, superseded by `SQLString` /
`SQLIdentifier` / `SQLQuotedIdentifier`.

The genuinely removed ones — the compile errors — are `LogicalType::SetAlias`,
`UnaryExecutor`/`BinaryExecutor`/`TernaryExecutor::ExecuteWithNulls`, and the
public fields of change 6.

**Three things that look like they need porting and do not.** These read
identically to the broken cases in a grep, so they are easy to over-port:

- `BaseExpression::SetAlias` still **exists** on main; it just takes an
  `Identifier` now, which is implicit from `const char *`. Only
  `LogicalType::SetAlias` was removed. A grep for `SetAlias` finds both.
- `BoundStatement::names` is `vector<Identifier>` on main, so
  `expr->SetAlias(bound.names[i])` compiles unchanged on both versions.
- `FunctionSet<T>::name` is still a public member (an `Identifier`). Only
  `functions` became `shared_ptr<const T>` (change 5).
- **`ConstantVector` did not get a `GetDataMutable`.** It kept a const-overloaded
  pair instead — `GetData(const Vector &)` returning `const T *` and
  `GetData(Vector &)` returning `T *` — so `ConstantVector` write sites need no
  change. Parameterising the shim on the accessor class handles this correctly:
  the probe is false, it falls back to `GetData`, and `GetData` on a non-const
  `Vector &` already returns a mutable pointer. Do not "fix" these into a
  `ConstantVector::GetDataMutable` that does not exist.
- `named_parameter_map_t` is now `identifier_map_t<Value>`, so `kv.first` in a
  named-parameter loop is an `Identifier` — and still needs no change.
  `Identifier` has `operator==` against `const char *`, `const string &` and
  `Identifier`, and `fn.named_parameters["literal"] = type` still works through
  the implicit literal constructor.

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

## You cannot test the v2.0 half locally

Your submodule is the stable line, so a local build only proves the v1.5 half.
The v2.0 half is verified by CI, and the loop is ~20 minutes.

The reverse is also true and worth using: **the stable leg of your own pipeline
is CI-grade evidence for the v1.5 half.** It builds against the pinned release
*and runs the suite*, on more platforms than you have locally. If local build
capacity is scarce, a green stable leg stands in for the local run — and its
Windows/MSVC job is the only thing in the matrix that will catch template
deduction that GCC and AppleClang accept.

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

**A run can sit in `queued` for over an hour, and `--json status` reads
`queued` the whole time** — so a naive `until status == completed` loop blocks
indefinitely with no signal at all.

The cause is usually account-wide runner backlog rather than anything about your
workflow, and it is self-inflicted if you are porting several repos at once: the
concurrent ports contend for one runner quota. Before concluding a run is stuck,
check whether *anything* is progressing:

```
gh api '/repos/OWNER/REPO/actions/runs?status=in_progress' --jq .total_count
```

Zero means backlog, not a broken job. (An earlier draft of this guide claimed the
canary leg specifically gets starved while the stable leg runs — that was wrong,
and it would send you hunting a workflow bug that does not exist. The two legs
were simply caught by the same backlog at different moments.)

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

**`gh pr edit --body-file` can fail silently on these repos.** It errors with a
GraphQL *"Projects (classic) is being deprecated ... (repository.pullRequest.projectCards)"*
and **leaves the body unchanged** — so a PR you believe you corrected still shows
the old text. Two ports hit this independently. Use the REST endpoint instead,
and re-read the body afterwards either way:

```
gh api -X PATCH repos/OWNER/REPO/pulls/N -F body=@body.md
```

**Reading the failure needs the REST API, not `gh run view`.** Because the canary
job calls a *reusable* workflow, `gh run view <run> --log-failed` prints nothing
at all — which reads exactly like a job that produced no errors. Get the job id
first, then fetch its log directly:

```
gh run view <run-id> --json jobs \
   --jq '.jobs[] | select(.conclusion=="failure") | "\(.databaseId) \(.name)"'
gh api repos/:owner/:repo/actions/jobs/<job-id>/logs | grep 'error:'
```

**Verify the dispatch actually happened.** `gh` shares one *user-level* REST
rate limit, and several concurrent ports polling run status will exhaust it. When
it goes, you get `HTTP 403: API rate limit exceeded` — and it takes out
`gh workflow run` too. The failure mode is nasty: your `git push` went over SSH
and succeeded, but the dispatch was refused, so you are left with a **push run
that does not include the `workflow_dispatch`-gated canary**. It looks exactly
like a slow queue. After dispatching, confirm a `workflow_dispatch` run exists
before waiting on one, and widen poll intervals when several ports share a
limit.

**A coordinator watching every repo is usually the biggest consumer.** A status
sweep across fourteen repos costs two API calls each; on a 90-second loop that is
~1,100 calls an hour on its own, before any of the ports poll at all. Prefer one
long-lived waiter per run over repeated sweeps, and treat an empty status sweep
as suspect until you have checked it is not a 403 — a rate-limited query renders
as an empty result and is indistinguishable from "no runs exist".

**And `gh api /rate_limit` will tell you nothing is wrong.** The limit you hit
this way is GitHub's *secondary* (abuse-detection) limit, which throttles request
*rate* rather than hourly volume — and it is not reported in `/rate_limit`, which
happily shows `core: 5000/5000` while every real call returns 403. Do not
conclude from a healthy `/rate_limit` that the 403 was something else. Back off
on wall-clock time; it clears on its own.

**Push and dispatch on the same commit cancel each other — and the wrong one
survives.** The `concurrency:` group covers the workflow, the ref *and* the SHA,
so a `push` run and a `workflow_dispatch` run on the **same commit** share a
group. The survivor may be the **push** run, which on a feature branch has the
canary gated OFF — so it schedules no canary at all and looks like a slow queue
rather than a mistake.

Two orderings work:

```
push  ->  cancel the push run  ->  dispatch          # what to do if you must push
dispatch on a commit you already pushed, then leave the branch alone
```

Runs on *different* SHAs do **not** cancel each other, so a stale earlier run
also sits in the queue eating runners until you cancel it explicitly. When
porting several repos at once, that stale-run backlog is self-inflicted and
worth clearing:

```
gh run list --workflow W.yml --branch B --limit 25 --json databaseId,status \
  --jq '.[]|select(.status=="queued" or .status=="in_progress")|.databaseId'
```

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

**Check your repo's format baseline before trusting — or distrusting — the
formatter.** clang-format 11.0.1 (the version `duckdb/scripts/format.py` demands)
reports drift on a pristine default branch in *some* of these repos but not
others; markdown and func_apply are both zero-drift. One line tells you which
you are in:

```
clang-format --style=file path/to/file | diff -u path/to/file -
```

Where the baseline is **clean**, verify your own diff is format-clean too — a
repo whose CI runs the format check goes red otherwise, and a shim block copied
from another repo is a common way to import drift. Where the baseline is
**dirty**, do not run `make format` as a pre-push step: it mixes unrelated
reformatting into your port and makes the review worthless. Check only that your
branch adds no *new* drift.

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

**`~/.duckdb/extensions/` is shared too — and a red test there may be real.** A
local `INSTALL` from one port replaces a binary every other port on the box
loads. One repo's tests went red on a function name it had never heard of, with
nothing wrong in its own tree.

Note what a normal cycle does *not* do: `make test_release` never installs
anything, because `EXTENSION_STATIC_BUILD=1` links the extension into the
unittest binary. So routine build-and-test cannot cause this — someone installed
deliberately.

And do not assume contamination means *wrong*. In our case the installed binary
was built from a sibling extension's `main`, which carried an **unreleased
breaking rename**. CI was green because `INSTALL` pulls the *published*
extension; the local run was red because it had the unpublished one. Both were
correct; they tested different upstream versions. Reinstalling the older artifact
would have made the test pass while hiding real drift until release day. When a
cross-extension test goes red, establish *which version* each side is testing
before you decide anything is broken.

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
