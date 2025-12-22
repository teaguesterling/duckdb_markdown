#pragma once

#include <vector>
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "markdown_utils.hpp"

namespace duckdb {

class TableRef;
struct ReplacementScanData;

/**
 * @brief Markdown Reader class for handling Markdown files in DuckDB
 * 
 * This class provides functions to read Markdown files into DuckDB tables. It supports:
 * - Single files, file lists, glob patterns, and directory paths
 * - Section-based reading for document analysis
 * - Metadata and content extraction
 * - Full-text search optimization
 */
class MarkdownReader {

public:
    // Structure to hold Markdown read options
    struct MarkdownReadOptions {
        bool extract_metadata = true;           // Whether to extract frontmatter metadata
        bool normalize_content = true;          // Whether to normalize Markdown content
        bool include_stats = false;             // Whether to calculate document statistics
        idx_t maximum_file_size = 16777216;     // 16MB default maximum file size
        markdown_utils::MarkdownFlavor flavor = markdown_utils::MarkdownFlavor::GFM;
        
        // Column inclusion options
        bool include_filepath = false;          // Whether to include file_path column
        bool content_as_varchar = false;        // Whether content should be varchar instead of markdown
        
        // Section reader specific
        bool include_content = true;            // Whether to include section content
        int32_t min_level = 1;                  // Minimum heading level
        int32_t max_level = 6;                  // Maximum heading level
        bool include_empty_sections = false;    // Whether to include sections without content

        // Content mode options (Issue #8)
        std::string content_mode = "minimal";   // "minimal", "full", or "smart"
        int32_t max_depth = 6;                  // Maximum depth to include (relative to min_level)
        idx_t max_content_length = 0;           // For smart mode (0 = auto, uses 2000 chars)
        std::string section_filter = "";        // Fragment filter (#section-id)

        // User-specified column types
        vector<string> column_names;            // User-provided column names
        vector<LogicalType> column_types;       // User-provided column types
    };

    /**
     * @brief Register the Markdown reader functions with DuckDB
     *
     * Registers the read_markdown and read_markdown_sections table functions
     *
     * @param loader The extension loader to register the functions with
     */
    static void RegisterFunction(ExtensionLoader &loader);

    /**
     * @brief Replace a markdown file string with 'read_markdown'
     * 
     * Performs a ReadReplacement on any paths containing .md or .markdown
     * 
     * @param context Client context for the query
     * @param input Input expression for replacement
     * @param data Additional data for the replacement scan
     */
    static unique_ptr<TableRef> ReadMarkdownReplacement(ClientContext &context, 
                                                        ReplacementScanInput &input,
                                                        optional_ptr<ReplacementScanData> data);

private:
    /**
     * @brief Bind function for read_markdown that returns whole documents
     * 
     * Returns one row per file with file_path, content (MD type), and optional metadata
     * 
     * @param context Client context for the query
     * @param input Bind input parameters
     * @param return_types Types of columns to return
     * @param names Names of columns to return
     * @return Function data for execution
     */
    static unique_ptr<FunctionData> MarkdownReadDocumentsBind(ClientContext &context,
                                                              TableFunctionBindInput &input,
                                                              vector<LogicalType> &return_types,
                                                              vector<string> &names);

    /**
     * @brief Execution function for read_markdown
     * 
     * @param context Client context
     * @param input Execution input data
     * @param state Local state for the function
     * @param output Output chunk to write results to
     */
    static void MarkdownReadDocumentsFunction(ClientContext &context,
                                             TableFunctionInput &input,
                                             DataChunk &output);

    /**
     * @brief Bind function for read_markdown_sections
     * 
     * Returns one row per section with detailed section information
     * 
     * @param context Client context for the query
     * @param input Bind input parameters
     * @param return_types Types of columns to return
     * @param names Names of columns to return
     * @return Function data for execution
     */
    static unique_ptr<FunctionData> MarkdownReadSectionsBind(ClientContext &context,
                                                             TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types,
                                                             vector<string> &names);

    /**
     * @brief Execution function for read_markdown_sections
     * 
     * @param context Client context
     * @param input Execution input data
     * @param state Local state for the function
     * @param output Output chunk to write results to
     */
    static void MarkdownReadSectionsFunction(ClientContext &context,
                                            TableFunctionInput &input,
                                            DataChunk &output);

    /**
     * @brief Get file paths from various input types (single file, list, glob, directory)
     * 
     * @param context Client context for file operations
     * @param path_value The input value containing file path(s)
     * @param ignore_errors Whether to ignore missing files
     * @return vector<string> List of resolved file paths
     */
    static vector<string> GetFiles(ClientContext &context, const Value &path_value, bool ignore_errors);

    /**
     * @brief Get files from glob pattern with cross-filesystem support
     * 
     * @param context Client context for file operations
     * @param pattern Glob pattern to match
     * @return vector<string> List of files matching the pattern
     */
    static vector<string> GetGlobFiles(ClientContext &context, const string &pattern);

    /**
     * @brief Read a Markdown file and parse it
     *
     * @param context Client context for file operations
     * @param file_path Path to the Markdown file
     * @param options Markdown read options
     * @return string The file content
     */
    static string ReadMarkdownFile(ClientContext &context, const string &file_path,
                                  const MarkdownReadOptions &options);

    /**
     * @brief Process a Markdown document into sections
     * 
     * @param content The Markdown content
     * @param options Read options
     * @return vector<markdown_utils::MarkdownSection> Parsed sections
     */
    static vector<markdown_utils::MarkdownSection> ProcessSections(const string &content, 
                                                                  const MarkdownReadOptions &options);

    /**
     * @brief Bind the columns parameter for explicit type specification
     * 
     * @param context Client context
     * @param input Function bind input
     * @param options Markdown read options to update with column specifications
     */
    static void BindColumnTypes(ClientContext &context, TableFunctionBindInput &input, 
                               MarkdownReadOptions &options);
};

} // namespace duckdb
