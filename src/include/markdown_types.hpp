#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

class MarkdownTypes {
public:
	//! The MARKDOWN type used by DuckDB (implemented as VARCHAR)
	static LogicalType MarkdownType();

	//! The duck_block STRUCT type for unified block/inline element representation
	//! STRUCT(kind VARCHAR, element_type VARCHAR, content VARCHAR, level INTEGER,
	//!        encoding VARCHAR, attributes MAP(VARCHAR, VARCHAR), element_order INTEGER)
	//! kind: 'block' or 'inline'
	//! element_type: 'heading', 'paragraph', 'bold', 'link', etc.
	//! Note: Type is defined by duck_block_utils extension; we just use the shape
	static LogicalType DuckBlockType();

	//! Register the MARKDOWN type and conversion functions
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
