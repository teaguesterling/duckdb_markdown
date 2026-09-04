#include "markdown_copy.hpp"
#include "duckdb_compat.hpp"
#include "duck_block_vocabulary.hpp"
#include "duck_block_functions.hpp"
#include "markdown_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"

namespace duckdb {

using Vocab = DuckBlockVocabulary;

//===--------------------------------------------------------------------===//
// WriteMarkdownBindData
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> WriteMarkdownBindData::Copy() const {
	auto result = make_uniq<WriteMarkdownBindData>();
	result->markdown_mode = markdown_mode;
	result->null_value = null_value;
	result->header = header;
	result->escape_pipes = escape_pipes;
	result->escape_newlines = escape_newlines;
	result->frontmatter = frontmatter;
	result->content_column = content_column;
	result->title_column = title_column;
	result->level_column = level_column;
	result->content_mode = content_mode;
	result->blank_lines = blank_lines;
	result->kind_column = kind_column;
	result->element_type_column = element_type_column;
	result->encoding_column = encoding_column;
	result->attributes_column = attributes_column;
	result->level_col_idx = level_col_idx;
	result->title_col_idx = title_col_idx;
	result->content_col_idx = content_col_idx;
	result->kind_col_idx = kind_col_idx;
	result->element_type_col_idx = element_type_col_idx;
	result->encoding_col_idx = encoding_col_idx;
	result->attributes_col_idx = attributes_col_idx;
	result->alignments = alignments;
	result->column_names = column_names;
	result->column_types = column_types;
	return std::move(result);
}

bool WriteMarkdownBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<WriteMarkdownBindData>();
	return markdown_mode == other.markdown_mode && null_value == other.null_value && header == other.header &&
	       escape_pipes == other.escape_pipes && escape_newlines == other.escape_newlines &&
	       frontmatter == other.frontmatter && content_column == other.content_column &&
	       title_column == other.title_column && level_column == other.level_column &&
	       content_mode == other.content_mode && blank_lines == other.blank_lines;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Register(ExtensionLoader &loader) {
	CopyFunction func("markdown");
	func.extension = "md";

	func.copy_to_bind = Bind;
	func.copy_to_initialize_global = InitializeGlobal;
	func.copy_to_initialize_local = InitializeLocal;
	func.copy_to_sink = Sink;
	func.copy_to_combine = Combine;
	func.copy_to_finalize = Finalize;
	func.copy_options = CopyOptions;

	loader.RegisterFunction(func);
}

//===--------------------------------------------------------------------===//
// Copy Options
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::CopyOptions(ClientContext &context, CopyOptionsInput &input) {
	// Core options
	input.options["markdown_mode"] = CopyOption(LogicalType::VARCHAR);
	input.options["null_value"] = CopyOption(LogicalType::VARCHAR);

	// Table mode options
	input.options["header"] = CopyOption(LogicalType::BOOLEAN);
	input.options["escape_pipes"] = CopyOption(LogicalType::BOOLEAN);
	input.options["escape_newlines"] = CopyOption(LogicalType::BOOLEAN);

	// Document mode options
	input.options["frontmatter"] = CopyOption(LogicalType::VARCHAR);
	input.options["content_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["title_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["level_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["content_mode"] = CopyOption(LogicalType::VARCHAR);
	input.options["blank_lines"] = CopyOption(LogicalType::INTEGER);

	// Blocks mode options (uses duck_block naming)
	input.options["kind_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["element_type_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["encoding_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["attributes_column"] = CopyOption(LogicalType::VARCHAR);
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownCopyFunction::Bind(ClientContext &context, CopyFunctionBindInput &input,
                                                    const vector<CompatName> &names,
                                                    const vector<LogicalType> &sql_types) {
	auto result = make_uniq<WriteMarkdownBindData>();
	auto &options = input.info.options;

	// Store schema info. Converted element-wise rather than assigned: on DuckDB
	// v2.0 `names` is a vector<Identifier>, and Identifier does not implicitly
	// convert to string -- deliberately, because it carries case-insensitive
	// comparison semantics that a silent conversion would discard.
	result->column_names.reserve(names.size());
	for (auto &name : names) {
		result->column_names.push_back(CompatNameStr(name));
	}
	result->column_types = sql_types;

	// Parse options
	for (auto &option : options) {
		auto loption = StringUtil::Lower(option.first);
		auto &value = option.second;

		if (loption == "markdown_mode") {
			auto mode_str = StringUtil::Lower(StringValue::Get(value[0]));
			if (mode_str == "table") {
				result->markdown_mode = WriteMarkdownBindData::MarkdownMode::TABLE;
			} else if (mode_str == "document") {
				result->markdown_mode = WriteMarkdownBindData::MarkdownMode::DOCUMENT;
			} else if (mode_str == "blocks" || mode_str == "duck_block") {
				result->markdown_mode = WriteMarkdownBindData::MarkdownMode::BLOCKS;
			} else {
				throw InvalidInputException(
				    "Invalid markdown_mode: '%s'. Expected 'table', 'document', 'blocks', or 'duck_block'", mode_str);
			}
		} else if (loption == "null_value") {
			result->null_value = StringValue::Get(value[0]);
		} else if (loption == "header") {
			result->header = BooleanValue::Get(value[0]);
		} else if (loption == "escape_pipes") {
			result->escape_pipes = BooleanValue::Get(value[0]);
		} else if (loption == "escape_newlines") {
			result->escape_newlines = BooleanValue::Get(value[0]);
		} else if (loption == "frontmatter") {
			result->frontmatter = StringValue::Get(value[0]);
		} else if (loption == "content_column") {
			result->content_column = StringValue::Get(value[0]);
		} else if (loption == "title_column") {
			result->title_column = StringValue::Get(value[0]);
		} else if (loption == "level_column") {
			result->level_column = StringValue::Get(value[0]);
		} else if (loption == "content_mode") {
			result->content_mode = StringUtil::Lower(StringValue::Get(value[0]));
		} else if (loption == "blank_lines") {
			result->blank_lines = IntegerValue::Get(value[0]);
		} else if (loption == "kind_column") {
			result->kind_column = StringValue::Get(value[0]);
		} else if (loption == "element_type_column") {
			result->element_type_column = StringValue::Get(value[0]);
		} else if (loption == "encoding_column") {
			result->encoding_column = StringValue::Get(value[0]);
		} else if (loption == "attributes_column") {
			result->attributes_column = StringValue::Get(value[0]);
		}
	}

	// For table mode, detect alignments
	if (result->markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		for (const auto &type : sql_types) {
			result->alignments.push_back(DetectAlignment(type));
		}
	}

	// For document mode, resolve column indices
	if (result->markdown_mode == WriteMarkdownBindData::MarkdownMode::DOCUMENT) {
		for (idx_t i = 0; i < names.size(); i++) {
			auto lower_name = StringUtil::Lower(names[i]);
			if (lower_name == StringUtil::Lower(result->level_column)) {
				result->level_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->title_column)) {
				result->title_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->content_column)) {
				result->content_col_idx = i;
			}
		}

		// Validate required columns for document mode
		if (result->level_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Document mode requires a '%s' column", result->level_column);
		}
		if (result->title_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Document mode requires a '%s' column", result->title_column);
		}
		// content_column is optional - sections can have empty content
	}

	// For blocks mode, resolve column indices (uses duck_block naming)
	if (result->markdown_mode == WriteMarkdownBindData::MarkdownMode::BLOCKS) {
		for (idx_t i = 0; i < names.size(); i++) {
			auto lower_name = StringUtil::Lower(names[i]);
			if (lower_name == StringUtil::Lower(result->kind_column)) {
				result->kind_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->element_type_column)) {
				result->element_type_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->content_column)) {
				result->content_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->level_column)) {
				result->level_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->encoding_column)) {
				result->encoding_col_idx = i;
			} else if (lower_name == StringUtil::Lower(result->attributes_column)) {
				result->attributes_col_idx = i;
			}
		}

		// Validate required columns for blocks mode
		if (result->element_type_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Blocks mode requires a '%s' column", result->element_type_column);
		}
		if (result->content_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException("Blocks mode requires a '%s' column", result->content_column);
		}
		// level, encoding, attributes are optional
	}

	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Initialize Global
//===--------------------------------------------------------------------===//

unique_ptr<GlobalFunctionData> MarkdownCopyFunction::InitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                                      const string &file_path) {
	auto result = make_uniq<WriteMarkdownGlobalState>();
	auto &fs = FileSystem::GetFileSystem(context);

	// Open file for writing
	result->handle =
	    fs.OpenFile(file_path, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE_NEW);

	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Initialize Local
//===--------------------------------------------------------------------===//

unique_ptr<LocalFunctionData> MarkdownCopyFunction::InitializeLocal(ExecutionContext &context,
                                                                    FunctionData &bind_data) {
	return make_uniq<WriteMarkdownLocalState>();
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Sink(ExecutionContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate_p,
                                LocalFunctionData &lstate_p, DataChunk &input) {
	auto &bind_data = bind_data_p.Cast<WriteMarkdownBindData>();
	auto &gstate = gstate_p.Cast<WriteMarkdownGlobalState>();
	auto &lstate = lstate_p.Cast<WriteMarkdownLocalState>();

	if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		// Table mode: render rows
		for (idx_t row_idx = 0; row_idx < input.size(); row_idx++) {
			lstate.buffer += RenderTableRow(input, row_idx, bind_data);
		}
	} else if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::DOCUMENT) {
		// Document mode: render sections
		for (idx_t row_idx = 0; row_idx < input.size(); row_idx++) {
			// Get level
			int32_t level = 1;
			if (bind_data.level_col_idx != DConstants::INVALID_INDEX) {
				auto level_val = input.data[bind_data.level_col_idx].GetValue(row_idx);
				if (!level_val.IsNull()) {
					level = level_val.GetValue<int32_t>();
				}
			}

			// Get title
			string title;
			if (bind_data.title_col_idx != DConstants::INVALID_INDEX) {
				auto title_val = input.data[bind_data.title_col_idx].GetValue(row_idx);
				if (!title_val.IsNull()) {
					title = title_val.ToString();
				}
			}

			// Get content
			string content;
			if (bind_data.content_col_idx != DConstants::INVALID_INDEX) {
				auto content_val = input.data[bind_data.content_col_idx].GetValue(row_idx);
				if (!content_val.IsNull()) {
					content = content_val.ToString();
				}
			}

			lstate.buffer += RenderSection(level, title, content, bind_data);
		}
	} else {
		// Blocks mode: render individual blocks
		for (idx_t row_idx = 0; row_idx < input.size(); row_idx++) {
			// Get kind (optional, defaults to 'block')
			string kind = "block";
			if (bind_data.kind_col_idx != DConstants::INVALID_INDEX) {
				auto kind_val = input.data[bind_data.kind_col_idx].GetValue(row_idx);
				if (!kind_val.IsNull()) {
					kind = kind_val.ToString();
				}
			}

			// Get element_type (required)
			string element_type;
			auto element_type_val = input.data[bind_data.element_type_col_idx].GetValue(row_idx);
			if (!element_type_val.IsNull()) {
				element_type = element_type_val.ToString();
			}

			// Get content (required)
			string content;
			auto content_val = input.data[bind_data.content_col_idx].GetValue(row_idx);
			if (!content_val.IsNull()) {
				content = content_val.ToString();
			}

			// Get level (optional). NULL is PRESERVED rather than defaulted: the
			// shared renderer reads an absent level as the top level, and a
			// sentinel like -1 would be read as a real depth and compare wrong.
			Value level = Value(LogicalType::INTEGER);
			if (bind_data.level_col_idx != DConstants::INVALID_INDEX) {
				auto level_val = input.data[bind_data.level_col_idx].GetValue(row_idx);
				if (!level_val.IsNull()) {
					level = Value::INTEGER(level_val.GetValue<int32_t>());
				}
			}

			// Get encoding (optional)
			string encoding = Vocab::ENCODING_TEXT;
			if (bind_data.encoding_col_idx != DConstants::INVALID_INDEX) {
				auto encoding_val = input.data[bind_data.encoding_col_idx].GetValue(row_idx);
				if (!encoding_val.IsNull()) {
					encoding = encoding_val.ToString();
				}
			}

			// Get attributes (optional). TYPED null when absent -- an untyped one
			// makes the struct's type differ row to row, and the list below would
			// then fail to build rather than degrade.
			Value attributes = Value(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
			if (bind_data.attributes_col_idx != DConstants::INVALID_INDEX) {
				auto attributes_val = input.data[bind_data.attributes_col_idx].GetValue(row_idx);
				if (!attributes_val.IsNull()) {
					attributes = attributes_val;
				}
			}

			// Rebuild the duck_block and hand it to the SHARED renderer in Combine.
			// element_order is NULL because this mode never bound the column: order
			// here is row order, which is what the renderer's walk uses anyway.
			child_list_t<Value> fields;
			fields.emplace_back("kind", Value(kind));
			fields.emplace_back("element_type", Value(element_type));
			fields.emplace_back("content", Value(content));
			fields.emplace_back("level", level);
			fields.emplace_back("encoding", Value(encoding));
			fields.emplace_back("attributes", attributes);
			fields.emplace_back("element_order", Value(LogicalType::INTEGER));
			lstate.elements.push_back(Value::STRUCT(std::move(fields)));
		}
	}
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Combine(ExecutionContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate_p,
                                   LocalFunctionData &lstate_p) {
	auto &bind_data = bind_data_p.Cast<WriteMarkdownBindData>();
	auto &gstate = gstate_p.Cast<WriteMarkdownGlobalState>();
	auto &lstate = lstate_p.Cast<WriteMarkdownLocalState>();

	if (!lstate.elements.empty()) {
		// ONE renderer for duck_blocks, shared with duck_blocks_to_md. This path
		// used to have its own row-at-a-time walk, which could not absorb inline
		// runs and rendered `# H` + empty paragraph + inlines with two spurious
		// blank lines between heading and body.
		lstate.buffer += DuckBlockFunctions::RenderDuckBlocksToMarkdown(
		    Value::LIST(MarkdownTypes::DuckBlockType(), std::move(lstate.elements)));
		lstate.elements.clear();
	}

	if (lstate.buffer.empty()) {
		return;
	}

	lock_guard<mutex> lock(gstate.write_lock);

	// Write header/frontmatter if not yet written
	if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		if (!gstate.header_written && bind_data.header) {
			string header_content = RenderTableHeader(bind_data);
			header_content += RenderTableSeparator(bind_data);
			gstate.handle->Write(header_content.data(), header_content.size());
			gstate.header_written = true;
		}
	} else {
		if (!gstate.frontmatter_written && !bind_data.frontmatter.empty()) {
			string fm_content = RenderFrontmatter(bind_data);
			gstate.handle->Write(fm_content.data(), fm_content.size());
			gstate.frontmatter_written = true;
		}
	}

	// Write buffered data
	gstate.handle->Write(lstate.buffer.data(), lstate.buffer.size());
	lstate.buffer.clear();
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//

void MarkdownCopyFunction::Finalize(ClientContext &context, FunctionData &bind_data_p, GlobalFunctionData &gstate_p) {
	auto &bind_data = bind_data_p.Cast<WriteMarkdownBindData>();
	auto &gstate = gstate_p.Cast<WriteMarkdownGlobalState>();

	// If no data was written but we need a header (empty table case)
	if (bind_data.markdown_mode == WriteMarkdownBindData::MarkdownMode::TABLE) {
		if (!gstate.header_written && bind_data.header) {
			string header_content = RenderTableHeader(bind_data);
			header_content += RenderTableSeparator(bind_data);
			gstate.handle->Write(header_content.data(), header_content.size());
		}
	} else {
		if (!gstate.frontmatter_written && !bind_data.frontmatter.empty()) {
			string fm_content = RenderFrontmatter(bind_data);
			gstate.handle->Write(fm_content.data(), fm_content.size());
		}
	}

	// Sync and close
	gstate.handle->Sync();
	gstate.handle->Close();
}

//===--------------------------------------------------------------------===//
// Table Mode Helpers
//===--------------------------------------------------------------------===//

string MarkdownCopyFunction::DetectAlignment(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return "right";
	case LogicalTypeId::BOOLEAN:
		return "center";
	default:
		return "left";
	}
}

string MarkdownCopyFunction::RenderTableHeader(const WriteMarkdownBindData &bind_data) {
	string result = "|";
	for (const auto &name : bind_data.column_names) {
		result += " " + name + " |";
	}
	result += "\n";
	return result;
}

string MarkdownCopyFunction::RenderTableSeparator(const WriteMarkdownBindData &bind_data) {
	string result = "|";
	for (const auto &alignment : bind_data.alignments) {
		if (alignment == "right") {
			result += "---:|";
		} else if (alignment == "center") {
			result += ":---:|";
		} else {
			result += "---|";
		}
	}
	result += "\n";
	return result;
}

string MarkdownCopyFunction::RenderTableRow(const DataChunk &chunk, idx_t row_idx,
                                            const WriteMarkdownBindData &bind_data) {
	string result = "|";
	for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); col_idx++) {
		auto value = chunk.data[col_idx].GetValue(row_idx);
		string cell_value;

		if (value.IsNull()) {
			cell_value = bind_data.null_value;
		} else {
			cell_value = value.ToString();
		}

		result += " " + EscapeCellValue(cell_value, bind_data) + " |";
	}
	result += "\n";
	return result;
}

string MarkdownCopyFunction::EscapeCellValue(const string &value, const WriteMarkdownBindData &bind_data) {
	string result = value;

	// Escape pipes
	if (bind_data.escape_pipes) {
		result = StringUtil::Replace(result, "|", "\\|");
	}

	// Escape newlines
	if (bind_data.escape_newlines) {
		result = StringUtil::Replace(result, "\n", "<br>");
		result = StringUtil::Replace(result, "\r", "");
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Document Mode Helpers
//===--------------------------------------------------------------------===//

string MarkdownCopyFunction::RenderFrontmatter(const WriteMarkdownBindData &bind_data) {
	if (bind_data.frontmatter.empty()) {
		return "";
	}
	return "---\n" + bind_data.frontmatter + "\n---\n\n";
}

string MarkdownCopyFunction::RenderSection(int32_t level, const string &title, const string &content,
                                           const WriteMarkdownBindData &bind_data) {
	string result;

	// Level 0 is treated as frontmatter content (if frontmatter option not set)
	if (level == 0) {
		// Level 0 sections output raw content (typically YAML frontmatter)
		if (!content.empty()) {
			result = "---\n" + content + "\n---\n\n";
		}
		return result;
	}

	// Regular heading (level 1-6)
	if (level > 0 && level <= 6 && !title.empty()) {
		result = string(level, '#') + " " + title + "\n";
	}

	// Content
	if (!content.empty()) {
		result += "\n" + content + "\n";
	}

	// Blank lines between sections
	for (int32_t i = 0; i < bind_data.blank_lines; i++) {
		result += "\n";
	}

	return result;
}

// The blocks-mode renderer that lived here -- RenderInlineElement,
// RenderBlockElement and RenderElement, ~340 lines -- is GONE. It was a second
// implementation of duck_block rendering, and it had drifted: rendering one
// element at a time, it could not absorb a block's inline run, so a paragraph
// with NULL content and its inline children came out as separate blocks with
// spurious blank lines between them. Sink now rebuilds the duck_block and
// Combine calls DuckBlockFunctions::RenderDuckBlocksToMarkdown -- the same
// renderer duck_blocks_to_md uses.
//
// Worth recording what deleting it FOUND: routing this path through the shared
// renderer immediately failed a test this path had always passed, because the
// shared renderer deleted the inline run after an `hr`. Two renderers meant each
// bug was invisible to the tests that would have caught it in the other.

} // namespace duckdb
