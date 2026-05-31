#pragma once

#include "duckdb.hpp"

// duckdb_compat.hpp — fleet-standard cross-version shim for DuckDB extensions.
//
// Pattern established by @bendrucker in teaguesterling/duckdb_webbed#76 (May 2026):
// detect the new API via __has_include of headers that moved in the same DuckDB
// refactor (PR #22377 et al — "mandatory per-vector size tracking" landed alongside
// the vector-buffer header reshuffle), then dispatch via a single #ifdef block.
//
// Cross-version coverage:
//   - duckdb v1.4.x / v1.5.x: old API everywhere
//   - duckdb main / v1.6.x:   new API everywhere
//
// See docs/DUCKDB_API_MIGRATION.md for the long-form story.

#if __has_include("duckdb/common/vector/list_vector.hpp")
#define DUCKDB_HAS_NEW_VECTOR_HEADERS 1
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#endif

namespace duckdb {

#ifdef DUCKDB_HAS_NEW_VECTOR_HEADERS

// --- Output chunk finalization ---
// DuckDB main mandates per-vector Size() tracking; DataChunk::SetCardinality only
// updates chunk.count. SetChildCardinality additionally calls FlatVector::SetSize
// on every column so query operators reading vec.Size() see the right value.
// Without this, VariadicExecutor (and similar) reports:
//   "Mismatch in input vector sizes ... expected 0 rows but got N"
inline void CompatSetOutputCardinality(DataChunk &chunk, idx_t count) {
	chunk.SetChildCardinality(count);
}

#else // Old API (v1.4.x / v1.5.x)

inline void CompatSetOutputCardinality(DataChunk &chunk, idx_t count) {
	chunk.SetCardinality(count);
}

#endif

// --- Catalog-aware Value-typed SetValue ---
// Cross-version helper. On duckdb main, VectorStringBuffer::SetValue and
// StandardVectorBuffer::SetValue fall back to Value::DefaultCastAs(target_type)
// when val.type() != column.type(). DefaultCastAs uses a stack-local
// CastFunctionSet that does NOT see extension-registered casts
// (loader.RegisterCastFunction); the cast silently returns NULL and SetValue
// writes NULL. Pre-casting via Value::CastAs(ClientContext&, target_type) uses
// the catalog's cast set, which includes extension casts. Behaves identically
// on v1.4.x / v1.5.x where the old SetValue tolerated alias-only mismatches.
inline void SetValueCasted(ClientContext &context, Vector &vec, idx_t idx, const Value &val) {
	vec.SetValue(idx, val.CastAs(context, vec.GetType()));
}

} // namespace duckdb
