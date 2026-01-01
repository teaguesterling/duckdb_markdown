#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

/**
 * @brief Markdown extraction table functions for extracting structured data from Markdown
 *
 * This class provides table functions for:
 * - Extracting code blocks (md_extract_code_blocks)
 * - Extracting links (md_extract_links)
 * - Extracting images (md_extract_images)
 * - Extracting headings (md_extract_headings)
 */
class MarkdownExtractionFunctions {
public:
	/**
	 * @brief Register all Markdown extraction table functions with DuckDB
	 *
	 * @param loader The extension loader to register the functions with
	 */
	static void Register(ExtensionLoader &loader);

private:
	// Bind data structures
	struct CodeBlocksBindData;
	struct LinksBindData;
	struct ImagesBindData;
	struct HeadingsBindData;

	// State structures
	struct CodeBlocksState;
	struct LinksState;
	struct ImagesState;
	struct HeadingsState;

	// Code blocks extraction
	static unique_ptr<FunctionData> CodeBlocksBind(ClientContext &context, TableFunctionBindInput &input,
	                                               vector<LogicalType> &return_types, vector<string> &names);
	static unique_ptr<LocalTableFunctionState> CodeBlocksInit(ExecutionContext &context, TableFunctionInitInput &input,
	                                                          GlobalTableFunctionState *global_state);
	static void CodeBlocksFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output);

	// Links extraction
	static unique_ptr<FunctionData> LinksBind(ClientContext &context, TableFunctionBindInput &input,
	                                          vector<LogicalType> &return_types, vector<string> &names);
	static unique_ptr<LocalTableFunctionState> LinksInit(ExecutionContext &context, TableFunctionInitInput &input,
	                                                     GlobalTableFunctionState *global_state);
	static void LinksFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output);

	// Images extraction
	static unique_ptr<FunctionData> ImagesBind(ClientContext &context, TableFunctionBindInput &input,
	                                           vector<LogicalType> &return_types, vector<string> &names);
	static unique_ptr<LocalTableFunctionState> ImagesInit(ExecutionContext &context, TableFunctionInitInput &input,
	                                                      GlobalTableFunctionState *global_state);
	static void ImagesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output);

	// Headings extraction
	static unique_ptr<FunctionData> HeadingsBind(ClientContext &context, TableFunctionBindInput &input,
	                                             vector<LogicalType> &return_types, vector<string> &names);
	static unique_ptr<LocalTableFunctionState> HeadingsInit(ExecutionContext &context, TableFunctionInitInput &input,
	                                                        GlobalTableFunctionState *global_state);
	static void HeadingsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output);

	// Registration sub-functions
	static void RegisterContentExtraction(ExtensionLoader &loader);
	static void RegisterStructureExtraction(ExtensionLoader &loader);
	static void RegisterSectionFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
