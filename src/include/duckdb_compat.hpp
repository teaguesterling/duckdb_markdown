#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include <type_traits>
#include <utility>

// Compatibility shims for building against BOTH the pinned stable DuckDB
// (v1.5.x, what this extension ships against) and DuckDB main (the v2.0 line,
// what community-extensions' `test_against_latest` builds against).
//
// FEATURE DETECTION, NOT VERSION NUMBERS. A version macro says when a thing
// changed; a probe says whether it changed here. The probe keeps working when a
// change is backported, reverted, or lands on a different branch than expected.
// (Idiom borrowed from duckdb_webbed's duckdb_compat.hpp, which the rest of the
// duck_block ecosystem already uses.)

// duckdb::Identifier replaced std::string as the name type in table-function and
// COPY bind signatures. Identifier compares case-insensitively, and construction
// from a RUNTIME string is explicit by design -- promoting a string to an
// identifier is meant to be a deliberate act at the call site -- so a boundary
// helper is needed rather than an implicit conversion.
#if __has_include("duckdb/common/identifier.hpp")
#define DUCKDB_HAS_IDENTIFIER 1
#include "duckdb/common/identifier.hpp"
#endif

// v2.0 split the per-vector accessor classes out of
// duckdb/common/types/vector.hpp into one header each under
// duckdb/common/vector/, and duckdb.hpp no longer pulls them in transitively.
// This header names FlatVector at namespace scope, so include it explicitly
// rather than relying on a transitive include that a given TU may not have.
// Presents otherwise as "'FlatVector' has not been declared", which reads like
// a missing symbol rather than a moved header.
#if __has_include("duckdb/common/vector/flat_vector.hpp")
#include "duckdb/common/vector/flat_vector.hpp"
#endif
#if __has_include("duckdb/common/vector/list_vector.hpp")
#include "duckdb/common/vector/list_vector.hpp"
#endif
#if __has_include("duckdb/common/vector/struct_vector.hpp")
#include "duckdb/common/vector/struct_vector.hpp"
#endif

namespace duckdb {

// --- bind-signature name type -------------------------------------------------
// Used wherever a bind callback receives or fills a vector of column names.
//
// DERIVED FROM DUCKDB'S OWN CONTAINER, NOT FROM A HEADER PROBE. The obvious
// probe -- #ifdef DUCKDB_HAS_IDENTIFIER -- is a TIME BOMB, because the presence
// of identifier.hpp and the type used in bind signatures are two different
// facts that have already come apart upstream:
//
//   v1.5-variegata @ b155d6f63c (our pin)  no identifier.hpp   bind: vector<string>
//   v1.5-variegata @ branch tip            HAS identifier.hpp  bind: vector<string>
//   main (v2.0)                            HAS identifier.hpp  bind: vector<Identifier>
//
// identifier.hpp was BACKPORTED to the stable branch without changing
// table_function_bind_t. So on the next submodule bump a header probe flips
// CompatName to Identifier on a DuckDB that still wants strings, and every bind
// signature in the extension stops compiling at once.
//
// TableFunctionBindInput::input_table_names has the same element type as the
// bind out-parameter on both lines (verified: table_function.hpp:110/288 on the
// pin, :123/319 on main), so asking it what the name type is cannot drift --
// it IS the thing that changed.
// Read the name type off table_function_bind_t ITSELF -- its fourth parameter is
// the vector being ported. Tighter than deriving from
// TableFunctionBindInput::input_table_names, which is a sibling that happens to
// move in step; here there is no "happens to" left, because the typedef named is
// the one that changed. Every bind signature then follows automatically.
template <class T>
struct CompatBindNamesOf;
template <class R, class A, class B, class C, class D>
struct CompatBindNamesOf<R (*)(A, B, C, D)> {
	using type = typename std::remove_reference<D>::type::value_type;
};
using CompatName = CompatBindNamesOf<table_function_bind_t>::type;

inline string CompatNameStr(const string &name) {
	return name;
}
#ifdef DUCKDB_HAS_IDENTIFIER
// Only declares the Identifier overload; it does NOT decide CompatName. Both
// overloads coexist happily when CompatName is still string.
inline string CompatNameStr(const Identifier &id) {
	return id.GetIdentifierName();
}
#endif

inline CompatName CompatMakeName(string name) {
	return CompatName(std::move(name));
}

// Ties the derived type to its overload set. Deriving CompatName fixes the type
// but leaves a second failure mode open: CompatName could resolve to Identifier
// on a DuckDB whose identifier.hpp this header did not find, so the Identifier
// overload above was never declared -- and then CompatNameStr either fails to
// match or silently picks a worse conversion. Assert the coupling instead of
// assuming it.
static_assert(std::is_same<decltype(CompatNameStr(std::declval<const CompatName &>())), string>::value,
              "CompatNameStr must accept the derived CompatName on every DuckDB line");

// --- LogicalType alias ---------------------------------------------------------
// v1.5: void SetAlias(string)      -- mutates in place
// v2.0: LogicalType WithAlias(string) const -- returns a copy, never mutating a
//       type whose type-info is shared.
//
// Detected by PROBING for the member rather than by the Identifier macro above,
// because these are two independent changes and tying one to the other would
// silently pick the wrong branch if they ever land in different releases.
// The member probe itself (the decltype(void(expr)) partial specialisation) is
// valid C++11; only the dispatch below needs care -- see the note on it.
template <class T, class = void>
struct CompatHasWithAlias : std::false_type {};
template <class T>
struct CompatHasWithAlias<T, decltype(void(std::declval<const T &>().WithAlias(string())))> : std::true_type {};

// Dispatched on a tag rather than with `if constexpr`, so the header also
// compiles at C++11. Several extensions in this ecosystem build their TUs at
// C++11 deliberately (forcing C++17 on the extension but not on libduckdb makes
// static-const members in duckdb's headers acquire implicit inline linkage in
// one and not the other, which produces multiple-definition link errors), and
// `if constexpr` is C++17-only. Tag dispatch has the same property that matters
// here: only the selected overload is instantiated, so the branch referring to
// the absent member is never compiled.
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE type, string alias, std::true_type) {
	return type.WithAlias(std::move(alias));
}
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE type, string alias, std::false_type) {
	type.SetAlias(std::move(alias));
	return type;
}
// The ENTRY POINT is deliberately NOT a template. A `template <class TYPE =
// LogicalType>` form looks equivalent but is not: the default template argument
// is inert because deduction wins, so the very common call
//
//     CompatWithAlias(LogicalType::VARCHAR, "md")
//
// deduces TYPE = LogicalTypeId -- `LogicalType::VARCHAR` is a static constexpr
// LogicalTypeId (types.hpp), not a LogicalType -- and then hard-errors inside
// the shim with "request for member 'SetAlias' in 'type', which is of non-class
// type 'duckdb::LogicalTypeId'". A concrete parameter restores the implicit
// LogicalTypeId -> LogicalType conversion at the call site. Only the Impl
// overloads stay templated, which is all the tag dispatch needs.
inline LogicalType CompatWithAlias(LogicalType type, string alias) {
	return CompatWithAliasImpl(std::move(type), std::move(alias), CompatHasWithAlias<LogicalType>());
}

// --- Vector::ToUnifiedFormat ---------------------------------------------------
// v1.5: ToUnifiedFormat(count, data)  -- the only overload
// v2.0: ToUnifiedFormat(data)         -- plus the count form kept as [[deprecated]]
//
// PROBE FOR THE COUNT-FREE OVERLOAD, not the count-taking one. v2.0 did not
// remove the count form, it deprecated it, so a probe for the count form is
// true on BOTH versions and the shim would always take the deprecated path --
// silently never calling the new API it exists to reach. The count-free form is
// the one that exists only on v2.0, so it is the one that discriminates.
template <class T, class = void>
struct CompatToUnifiedWithoutCount : std::false_type {};
template <class T>
struct CompatToUnifiedWithoutCount<T, decltype(void(std::declval<T &>().ToUnifiedFormat(
                                          std::declval<UnifiedVectorFormat &>())))> : std::true_type {};

template <class VEC>
inline void CompatToUnifiedFormatImpl(VEC &vec, idx_t, UnifiedVectorFormat &data, std::true_type) {
	vec.ToUnifiedFormat(data);
}
template <class VEC>
inline void CompatToUnifiedFormatImpl(VEC &vec, idx_t count, UnifiedVectorFormat &data, std::false_type) {
	vec.ToUnifiedFormat(count, data);
}
template <class VEC = Vector>
inline void CompatToUnifiedFormat(VEC &vec, idx_t count, UnifiedVectorFormat &data) {
	CompatToUnifiedFormatImpl(vec, count, data, CompatToUnifiedWithoutCount<VEC>());
}

// --- FlatVector mutable data ---------------------------------------------------
// v1.5: FlatVector::GetData<T>(vec)         returns T*
// v2.0: FlatVector::GetData<T>(vec)         returns const T*
//       FlatVector::GetDataMutable<T>(vec)  returns T*
// Writing through the v2.0 GetData is a compile error, which is the point of the
// split -- so the WRITE path must ask for mutability explicitly.
template <class T, class = void>
struct CompatHasFlatGetDataMutable : std::false_type {};
template <class T>
struct CompatHasFlatGetDataMutable<T, decltype(void(T::template GetDataMutable<bool>(std::declval<Vector &>())))>
    : std::true_type {};

template <class VALUE, class FV>
inline VALUE *CompatFlatDataMutableImpl(Vector &vec, std::true_type) {
	return FV::template GetDataMutable<VALUE>(vec);
}
template <class VALUE, class FV>
inline VALUE *CompatFlatDataMutableImpl(Vector &vec, std::false_type) {
	return FV::template GetData<VALUE>(vec);
}
template <class VALUE, class FV = FlatVector>
inline VALUE *CompatFlatDataMutable(Vector &vec) {
	return CompatFlatDataMutableImpl<VALUE, FV>(vec, CompatHasFlatGetDataMutable<FV>());
}

// --- FlatVector validity mask -----------------------------------------------
// v1.5: Validity(Vector &)              returns ValidityMask &
// v2.0: Validity(const Vector &)        returns const ValidityMask &
//       ValidityMutable(Vector &)       returns ValidityMask &
//
// Same copy-on-write split as GetData/GetDataMutable: ValidityMutable goes
// through BufferMutable() and un-shares, Validity goes through Buffer() and does
// not. Probed separately from the GetDataMutable change because they are two
// independent upstream changes.
//
// Worse to diagnose than the GetData case: `auto &m = FlatVector::Validity(v)`
// still COMPILES on v2.0, silently deducing a const reference. The error appears
// later, at the mutation, as "passing 'const duckdb::ValidityMask' as 'this'
// argument discards qualifiers" -- naming neither Validity nor FlatVector. Grep
// for the MUTATION (SetInvalid/SetValid/SetAllInvalid/SetAllValid) and walk back
// to where the reference was bound.
template <class T, class = void>
struct CompatHasFlatValidityMutable : std::false_type {};
template <class T>
struct CompatHasFlatValidityMutable<T, decltype(void(T::ValidityMutable(std::declval<Vector &>())))> : std::true_type {
};

template <class FV>
inline ValidityMask &CompatFlatValidityMutableImpl(Vector &vec, std::true_type) {
	return FV::ValidityMutable(vec);
}
template <class FV>
inline ValidityMask &CompatFlatValidityMutableImpl(Vector &vec, std::false_type) {
	return FV::Validity(vec);
}
template <class FV = FlatVector>
inline ValidityMask &CompatFlatValidityMutable(Vector &vec) {
	return CompatFlatValidityMutableImpl<FV>(vec, CompatHasFlatValidityMutable<FV>());
}

} // namespace duckdb
