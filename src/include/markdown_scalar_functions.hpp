#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

/**
 * @brief Markdown scalar functions for converting and validating Markdown content
 * 
 * This class provides scalar functions for:
 * - Converting Markdown to HTML (md_to_html)
 * - Converting Markdown to plain text (md_to_text) 
 * - Converting values to Markdown (value_to_md)
 * - Validating Markdown syntax (md_valid)
 * - Extracting metadata (md_extract_metadata)
 * - Calculating statistics (md_stats)
 */
class MarkdownFunctions {
public:
    /**
     * @brief Register all Markdown scalar functions with DuckDB
     * 
     * @param db The database instance to register the functions with
     */
    static void Register(DatabaseInstance &db);

private:
    static void RegisterValidationFunction(DatabaseInstance &db);
    static void RegisterConversionFunctions(DatabaseInstance &db);
    static void RegisterMarkdownTypeFunctions(DatabaseInstance &db);
    static void RegisterStatsFunctions(DatabaseInstance &db);
    static void RegisterMetadataFunctions(DatabaseInstance &db);
};

} // namespace duckdb