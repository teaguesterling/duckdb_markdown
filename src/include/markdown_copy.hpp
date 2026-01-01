#pragma once

#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/common/file_system.hpp"
#include <string>
#include <vector>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Markdown Copy Bind Data
//===--------------------------------------------------------------------===//

struct WriteMarkdownBindData : public FunctionData {
	// Mode (explicit, no auto-detect)
	enum class MarkdownMode { TABLE, DOCUMENT, BLOCKS };
	MarkdownMode markdown_mode = MarkdownMode::TABLE;

	// Common options
	string null_value = "";

	// Table mode options
	bool header = true;
	bool escape_pipes = true;
	bool escape_newlines = true;

	// Document mode options (mirror reader)
	string frontmatter = ""; // Raw YAML or empty
	string content_column = "content";
	string title_column = "title";
	string level_column = "level";
	string content_mode = "minimal"; // 'minimal' or 'full'
	int32_t blank_lines = 1;

	// Blocks mode column names (configurable)
	string block_type_column = "block_type";
	string encoding_column = "encoding";
	string attributes_column = "attributes";

	// Resolved schema info
	idx_t level_col_idx = DConstants::INVALID_INDEX;
	idx_t title_col_idx = DConstants::INVALID_INDEX;
	idx_t content_col_idx = DConstants::INVALID_INDEX;
	// Blocks mode column indices
	idx_t block_type_col_idx = DConstants::INVALID_INDEX;
	idx_t encoding_col_idx = DConstants::INVALID_INDEX;
	idx_t attributes_col_idx = DConstants::INVALID_INDEX;
	vector<string> alignments; // Per-column alignment for table mode
	vector<string> column_names;
	vector<LogicalType> column_types;

public:
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//===--------------------------------------------------------------------===//
// Markdown Copy Global State
//===--------------------------------------------------------------------===//

struct WriteMarkdownGlobalState : public GlobalFunctionData {
	//! File handle for writing
	unique_ptr<FileHandle> handle;
	//! Lock for thread-safe writes
	mutex write_lock;
	//! Whether the header has been written (table mode)
	bool header_written = false;
	//! Whether frontmatter has been written (document mode)
	bool frontmatter_written = false;
};

//===--------------------------------------------------------------------===//
// Markdown Copy Local State
//===--------------------------------------------------------------------===//

struct WriteMarkdownLocalState : public LocalFunctionData {
	//! Local buffer for accumulating output
	string buffer;
};

//===--------------------------------------------------------------------===//
// Markdown Copy Functions
//===--------------------------------------------------------------------===//

class MarkdownCopyFunction {
public:
	//! Register the markdown copy function
	static void Register(ExtensionLoader &loader);

	//! Bind function - parse options and set up schema info
	static unique_ptr<FunctionData> Bind(ClientContext &context, CopyFunctionBindInput &input,
	                                     const vector<string> &names, const vector<LogicalType> &sql_types);

	//! Initialize global state - open file for writing
	static unique_ptr<GlobalFunctionData> InitializeGlobal(ClientContext &context, FunctionData &bind_data,
	                                                       const string &file_path);

	//! Initialize local state - create buffer
	static unique_ptr<LocalFunctionData> InitializeLocal(ExecutionContext &context, FunctionData &bind_data);

	//! Sink function - write data chunks
	static void Sink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                 LocalFunctionData &lstate, DataChunk &input);

	//! Combine function - flush local buffers to global
	static void Combine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                    LocalFunctionData &lstate);

	//! Finalize function - close file
	static void Finalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate);

	//! Copy options registration
	static void CopyOptions(ClientContext &context, CopyOptionsInput &input);

private:
	//===--------------------------------------------------------------------===//
	// Table Mode Helpers
	//===--------------------------------------------------------------------===//

	//! Detect alignment based on column type
	static string DetectAlignment(const LogicalType &type);

	//! Render the header row for table mode
	static string RenderTableHeader(const WriteMarkdownBindData &bind_data);

	//! Render the separator row with alignment indicators
	static string RenderTableSeparator(const WriteMarkdownBindData &bind_data);

	//! Render a single data row for table mode
	static string RenderTableRow(const DataChunk &chunk, idx_t row_idx, const WriteMarkdownBindData &bind_data);

	//! Escape a cell value for table mode
	static string EscapeCellValue(const string &value, const WriteMarkdownBindData &bind_data);

	//===--------------------------------------------------------------------===//
	// Document Mode Helpers
	//===--------------------------------------------------------------------===//

	//! Render frontmatter YAML block
	static string RenderFrontmatter(const WriteMarkdownBindData &bind_data);

	//! Render a section heading and content
	static string RenderSection(int32_t level, const string &title, const string &content,
	                            const WriteMarkdownBindData &bind_data);

	//===--------------------------------------------------------------------===//
	// Blocks Mode Helpers
	//===--------------------------------------------------------------------===//

	//! Render a single block from flattened block representation
	static string RenderBlock(const string &block_type, const string &content, int32_t level, const string &encoding,
	                          const Value &attributes, const WriteMarkdownBindData &bind_data);
};

} // namespace duckdb
