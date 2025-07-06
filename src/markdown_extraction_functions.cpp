#include "markdown_extraction_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Simple placeholder extraction functions
//===--------------------------------------------------------------------===//

// For now, provide a simple register function that doesn't add extraction functions
// These can be implemented later when needed for the advanced examples
void MarkdownExtractionFunctions::Register(DatabaseInstance &db) {
    // TODO: Register extraction functions like md_extract_code_blocks, md_extract_links, etc.
    // These will be implemented after the basic reader functions are working
    
    // For now, just register the basic scalar functions that are already working
    // md_extract_section and md_section_breadcrumb are already registered in scalar functions
}

} // namespace duckdb