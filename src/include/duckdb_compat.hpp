#pragma once

#include "duckdb.hpp"
#include <type_traits>

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

namespace duckdb {

// --- bind-signature name type -------------------------------------------------
// Used wherever a bind callback receives or fills a vector of column names.
#ifdef DUCKDB_HAS_IDENTIFIER
using CompatName = Identifier;
inline string CompatNameStr(const Identifier &id) {
	return id.GetIdentifierName();
}
inline Identifier CompatMakeName(string name) {
	return Identifier(std::move(name));
}
#else
using CompatName = string;
inline string CompatNameStr(const string &name) {
	return name;
}
inline string CompatMakeName(string name) {
	return name;
}
#endif

// --- LogicalType alias ---------------------------------------------------------
// v1.5: void SetAlias(string)      -- mutates in place
// v2.0: LogicalType WithAlias(string) const -- returns a copy, never mutating a
//       type whose type-info is shared.
//
// Detected by PROBING for the member rather than by the Identifier macro above,
// because these are two independent changes and tying one to the other would
// silently pick the wrong branch if they ever land in different releases.
// `if constexpr` discards the untaken branch only inside a template, hence the
// template parameter.
template <class T, class = void>
struct CompatHasWithAlias : std::false_type {};
template <class T>
struct CompatHasWithAlias<T, decltype(void(std::declval<const T &>().WithAlias(string())))> : std::true_type {};

template <class TYPE = LogicalType>
inline LogicalType CompatWithAlias(TYPE type, string alias) {
	if constexpr (CompatHasWithAlias<TYPE>::value) {
		return type.WithAlias(std::move(alias));
	} else {
		type.SetAlias(std::move(alias));
		return type;
	}
}

// --- Vector::ToUnifiedFormat ---------------------------------------------------
// v2.0 dropped the count parameter. Probed the same way.
template <class T, class = void>
struct CompatToUnifiedTakesCount : std::false_type {};
template <class T>
struct CompatToUnifiedTakesCount<T, decltype(void(std::declval<T &>().ToUnifiedFormat(
                                        idx_t(0), std::declval<UnifiedVectorFormat &>())))> : std::true_type {};

template <class VEC = Vector>
inline void CompatToUnifiedFormat(VEC &vec, idx_t count, UnifiedVectorFormat &data) {
	if constexpr (CompatToUnifiedTakesCount<VEC>::value) {
		vec.ToUnifiedFormat(count, data);
	} else {
		vec.ToUnifiedFormat(data);
	}
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

template <class VALUE, class FV = FlatVector>
inline VALUE *CompatFlatDataMutable(Vector &vec) {
	if constexpr (CompatHasFlatGetDataMutable<FV>::value) {
		return FV::template GetDataMutable<VALUE>(vec);
	} else {
		return FV::template GetData<VALUE>(vec);
	}
}

} // namespace duckdb
