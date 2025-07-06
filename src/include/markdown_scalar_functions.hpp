#pragma once

#include "duckdb.hpp"

namespace duckdb {

class MarkdownFunctions {
public:
    static void Register(DatabaseInstance &db);

private:
    // Register basic Markdown validation and conversion functions
    static void RegisterValidationFunction(DatabaseInstance &db);
    
    // Register Markdown conversion functions (to HTML, text, etc.)
    static void RegisterConversionFunctions(DatabaseInstance &db);
    
    // Register Markdown type functions (md_to_html, md_to_text, value_to_md)
    static void RegisterMarkdownTypeFunctions(DatabaseInstance &db);
    
    // Register Markdown statistics functions
    static void RegisterStatsFunctions(DatabaseInstance &db);
    
    // Register Markdown metadata functions
    static void RegisterMetadataFunctions(DatabaseInstance &db);
};

} // namespace duckdb
