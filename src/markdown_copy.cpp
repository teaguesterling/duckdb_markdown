#include "markdown_copy.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"

namespace duckdb {

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
	result->element_type_column = element_type_column;
	result->encoding_column = encoding_column;
	result->attributes_column = attributes_column;
	result->level_col_idx = level_col_idx;
	result->title_col_idx = title_col_idx;
	result->content_col_idx = content_col_idx;
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
	input.options["element_type_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["encoding_column"] = CopyOption(LogicalType::VARCHAR);
	input.options["attributes_column"] = CopyOption(LogicalType::VARCHAR);
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownCopyFunction::Bind(ClientContext &context, CopyFunctionBindInput &input,
                                                    const vector<string> &names, const vector<LogicalType> &sql_types) {
	auto result = make_uniq<WriteMarkdownBindData>();
	auto &options = input.info.options;

	// Store schema info
	result->column_names = names;
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
			if (lower_name == StringUtil::Lower(result->element_type_column)) {
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

			// Get level (optional)
			int32_t level = -1;
			if (bind_data.level_col_idx != DConstants::INVALID_INDEX) {
				auto level_val = input.data[bind_data.level_col_idx].GetValue(row_idx);
				if (!level_val.IsNull()) {
					level = level_val.GetValue<int32_t>();
				}
			}

			// Get encoding (optional)
			string encoding = "text";
			if (bind_data.encoding_col_idx != DConstants::INVALID_INDEX) {
				auto encoding_val = input.data[bind_data.encoding_col_idx].GetValue(row_idx);
				if (!encoding_val.IsNull()) {
					encoding = encoding_val.ToString();
				}
			}

			// Get attributes (optional)
			Value attributes;
			if (bind_data.attributes_col_idx != DConstants::INVALID_INDEX) {
				attributes = input.data[bind_data.attributes_col_idx].GetValue(row_idx);
			}

			lstate.buffer += RenderBlock(element_type, content, level, encoding, attributes, bind_data);
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

//===--------------------------------------------------------------------===//
// Blocks Mode Helpers
//===--------------------------------------------------------------------===//

string MarkdownCopyFunction::RenderBlock(const string &element_type, const string &content, int32_t level,
                                         const string &encoding, const Value &attributes,
                                         const WriteMarkdownBindData &bind_data) {
	string result;

	// Helper to get attribute from MAP value
	auto get_attr = [&attributes](const string &key) -> string {
		if (attributes.IsNull() || attributes.type().id() != LogicalTypeId::MAP) {
			return "";
		}
		auto &map_children = MapValue::GetChildren(attributes);
		for (const auto &entry : map_children) {
			auto &entry_children = StructValue::GetChildren(entry);
			if (entry_children.size() == 2 && !entry_children[0].IsNull()) {
				if (entry_children[0].ToString() == key && !entry_children[1].IsNull()) {
					return entry_children[1].ToString();
				}
			}
		}
		return "";
	};

	if (element_type == "frontmatter" || element_type == "metadata") {
		// YAML frontmatter
		result = "---\n" + content + "\n---\n\n";
	} else if (element_type == "heading") {
		// ATX heading with level
		int32_t heading_level = level > 0 && level <= 6 ? level : 1;
		result = string(heading_level, '#') + " " + content + "\n\n";
	} else if (element_type == "paragraph") {
		// Plain paragraph
		result = content + "\n\n";
	} else if (element_type == "code") {
		// Fenced code block
		string language = get_attr("language");
		result = "```" + language + "\n" + content + "\n```\n\n";
	} else if (element_type == "blockquote") {
		// Block quote - add > prefix to each line
		string quoted;
		std::istringstream iss(content);
		std::string line;
		while (std::getline(iss, line)) {
			quoted += "> " + line + "\n";
		}
		result = quoted + "\n";
	} else if (element_type == "list") {
		// List - content is JSON encoded, need to decode
		// For now, output as-is if not JSON or parse if JSON
		if (encoding == "json" && content.length() > 2 && content[0] == '[') {
			// Simple JSON array parsing for list items
			// Format: ["item1", "item2", ...]
			bool ordered = get_attr("ordered") == "true";
			int start = 1;
			string start_str = get_attr("start");
			if (!start_str.empty()) {
				try {
					start = std::stoi(start_str);
				} catch (...) {
				}
			}

			// Parse JSON array (simple implementation)
			string items_str = content.substr(1, content.length() - 2); // Remove [ ]
			vector<string> items;
			string current_item;
			bool in_string = false;
			bool escape_next = false;

			for (size_t i = 0; i < items_str.length(); i++) {
				char c = items_str[i];
				if (escape_next) {
					if (c == 'n')
						current_item += '\n';
					else if (c == 't')
						current_item += '\t';
					else if (c == 'r')
						current_item += '\r';
					else
						current_item += c;
					escape_next = false;
				} else if (c == '\\') {
					escape_next = true;
				} else if (c == '"') {
					if (in_string) {
						items.push_back(current_item);
						current_item.clear();
					}
					in_string = !in_string;
				} else if (in_string) {
					current_item += c;
				}
			}

			// Render list items
			int item_num = start;
			for (const auto &item : items) {
				if (ordered) {
					result += std::to_string(item_num++) + ". " + item + "\n";
				} else {
					result += "- " + item + "\n";
				}
			}
			result += "\n";
		} else {
			// Fallback: output content directly
			result = content + "\n\n";
		}
	} else if (element_type == "table") {
		// Table - content is JSON encoded as {"headers": [...], "rows": [[...], ...]}
		if (encoding == "json" && content.find("\"headers\"") != string::npos) {
			// Parse JSON table format
			vector<string> headers;
			vector<vector<string>> rows;

			// Simple JSON parsing for table structure
			// Find headers array
			size_t headers_start = content.find("\"headers\":");
			if (headers_start != string::npos) {
				size_t arr_start = content.find('[', headers_start);
				size_t arr_end = content.find(']', arr_start);
				if (arr_start != string::npos && arr_end != string::npos) {
					string headers_str = content.substr(arr_start + 1, arr_end - arr_start - 1);
					// Parse header items
					bool in_string = false;
					bool escape_next = false;
					string current;
					for (char c : headers_str) {
						if (escape_next) {
							current += c;
							escape_next = false;
						} else if (c == '\\') {
							escape_next = true;
						} else if (c == '"') {
							if (in_string) {
								headers.push_back(current);
								current.clear();
							}
							in_string = !in_string;
						} else if (in_string) {
							current += c;
						}
					}
				}
			}

			// Find rows array
			size_t rows_start = content.find("\"rows\":");
			if (rows_start != string::npos) {
				size_t outer_start = content.find('[', rows_start);
				if (outer_start != string::npos) {
					// Parse each row
					size_t pos = outer_start + 1;
					while (pos < content.size()) {
						size_t row_start = content.find('[', pos);
						if (row_start == string::npos)
							break;
						size_t row_end = content.find(']', row_start);
						if (row_end == string::npos)
							break;

						string row_str = content.substr(row_start + 1, row_end - row_start - 1);
						vector<string> row;
						bool in_string = false;
						bool escape_next = false;
						string current;
						for (char c : row_str) {
							if (escape_next) {
								current += c;
								escape_next = false;
							} else if (c == '\\') {
								escape_next = true;
							} else if (c == '"') {
								if (in_string) {
									row.push_back(current);
									current.clear();
								}
								in_string = !in_string;
							} else if (in_string) {
								current += c;
							}
						}
						if (!row.empty()) {
							rows.push_back(row);
						}
						pos = row_end + 1;
					}
				}
			}

			// Render as markdown table
			if (!headers.empty()) {
				result = "|";
				for (const auto &h : headers) {
					result += " " + h + " |";
				}
				result += "\n|";
				for (size_t i = 0; i < headers.size(); i++) {
					result += "---|";
				}
				result += "\n";
				for (const auto &row : rows) {
					result += "|";
					for (const auto &cell : row) {
						result += " " + cell + " |";
					}
					result += "\n";
				}
				result += "\n";
			} else {
				result = content + "\n\n";
			}
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == "hr") {
		// Horizontal rule
		result = "---\n\n";
	} else if (element_type == "html" || element_type == "raw" || element_type == "md:html_block") {
		// Raw HTML
		result = content + "\n\n";
	} else {
		// Unknown block type - output content as-is
		result = content + "\n\n";
	}

	return result;
}

} // namespace duckdb
