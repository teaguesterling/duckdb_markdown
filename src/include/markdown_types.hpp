#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

class MarkdownTypes {
public:
    //! The MARKDOWN type used by DuckDB (implemented as VARCHAR)
    static LogicalType MarkdownType();
    
    //! Register the MARKDOWN type and conversion functions
    static void Register(DatabaseInstance &db);
};

} // namespace duckdb
