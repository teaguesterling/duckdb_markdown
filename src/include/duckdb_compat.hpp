//===--------------------------------------------------------------------===//
//
//  duckdb_compat.hpp — cross-version compatibility shims for duckdb_markdown
//
//  DuckDB's internal C++ API has churned non-trivially between v1.4.3 and
//  the post-`ffdceae563` main branch (April 2026). This header collects the
//  small SFINAE-detected helpers we use to stay buildable against BOTH
//  matrix entries in MainDistributionPipeline.yml.
//
//  Each helper is a single-source-of-truth replacement for a duckdb API
//  that moved. The pattern is: declare a `std::void_t`-based trait that
//  detects the new API at compile time, then dispatch via `if constexpr`.
//
//  See docs/DUCKDB_API_MIGRATION.md for the long-form rationale, the
//  symptoms each gap produced in our CI, and an upgrade checklist other
//  extensions can use.
//
//===--------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"

#include <type_traits>
#include <utility>

namespace duckdb {
namespace markdown_compat {

//===--------------------------------------------------------------------===//
// SetChildCardinality detection
//===--------------------------------------------------------------------===//
//
// `DataChunk::SetCardinality(N)` updates only `chunk.count` on both v1.4.3
// and main. On v1.4.3 that is sufficient — query operators consult the
// chunk's `count` field directly and individual vectors do not track their
// own size. On main (post the vector-buffer refactor that introduced
// `VectorBuffer::v_size`), some operators (e.g. `VariadicExecutor`) now
// check each vector's `Size()` *in addition to* the chunk's cardinality.
// `SetCardinality` does not propagate to children there, so vectors stay
// at `Size() == 0` even after we've populated their data via `SetValue`
// — producing
//
//   INTERNAL Error: Mismatch in input vector sizes for VariadicExecutor
//   — expected 0 rows but got N
//
// `DataChunk::SetChildCardinality(N)` (added in main) sets the chunk's
// count AND calls `FlatVector::SetSize` on every child vector. v1.4.3
// has no such method.
//
// This trait detects it at compile time; `SetCardinalityPropagating`
// dispatches to the right call.

template <typename T, typename = void>
struct has_set_child_cardinality : std::false_type {};

template <typename T>
struct has_set_child_cardinality<T, std::void_t<decltype(std::declval<T &>().SetChildCardinality(idx_t(0)))>>
    : std::true_type {};

//! Set the chunk's cardinality and propagate to each child vector's
//! independently-tracked Size() where the API supports it. Use this in
//! every table-function execute that ends with `output.SetCardinality(N)`.
template <typename Chunk>
inline void SetCardinalityPropagating(Chunk &chunk, idx_t count) {
	if constexpr (has_set_child_cardinality<Chunk>::value) {
		chunk.SetChildCardinality(count);
	} else {
		chunk.SetCardinality(count);
	}
}

} // namespace markdown_compat
} // namespace duckdb
