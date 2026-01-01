#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

class MarkdownTypes {
public:
	//! The MARKDOWN type used by DuckDB (implemented as VARCHAR)
	static LogicalType MarkdownType();

	//! The markdown_doc_block STRUCT type for block-level document representation
	//! STRUCT(block_type VARCHAR, content VARCHAR, level INTEGER, encoding VARCHAR,
	//!        attributes MAP(VARCHAR, VARCHAR), block_order INTEGER)
	static LogicalType MarkdownDocBlockType();

	//! Register the MARKDOWN type and conversion functions
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
