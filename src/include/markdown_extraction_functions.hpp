#pragma once

#include "duckdb/main/database.hpp"

namespace duckdb {

//! Markdown extraction functions for querying document content
class MarkdownExtractionFunctions {
public:
    static void Register(DatabaseInstance &db);

private:
    // Register content extraction functions
    static void RegisterContentExtraction(DatabaseInstance &db);
    
    // Register structure extraction functions
    static void RegisterStructureExtraction(DatabaseInstance &db);
    
    // Register section navigation functions
    static void RegisterSectionFunctions(DatabaseInstance &db);
};

} // namespace duckdb
