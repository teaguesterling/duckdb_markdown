#include "duck_block_functions.hpp"
#include "markdown_types.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/value.hpp"
#include <sstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::GetAttribute(const Value &attributes, const string &key) {
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
}

vector<string> DuckBlockFunctions::ParseJsonListItems(const string &content) {
	vector<string> items;
	if (content.length() < 2 || content[0] != '[') {
		return items;
	}

	string items_str = content.substr(1, content.length() - 2); // Remove [ ]
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

	return items;
}

void DuckBlockFunctions::ParseJsonTable(const string &content, vector<string> &headers, vector<vector<string>> &rows) {
	// Find headers array
	size_t headers_start = content.find("\"headers\":");
	if (headers_start != string::npos) {
		size_t arr_start = content.find('[', headers_start);
		size_t arr_end = content.find(']', arr_start);
		if (arr_start != string::npos && arr_end != string::npos) {
			string headers_str = content.substr(arr_start + 1, arr_end - arr_start - 1);
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
}

//===--------------------------------------------------------------------===//
// RenderBlockToMarkdown
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderBlockToMarkdown(const string &block_type, const string &content, int32_t level,
                                                 const string &encoding, const Value &attributes) {
	string result;

	if (block_type == "frontmatter" || block_type == "metadata") {
		// YAML frontmatter
		result = "---\n" + content + "\n---\n\n";
	} else if (block_type == "heading") {
		// ATX heading with level
		int32_t heading_level = level > 0 && level <= 6 ? level : 1;
		result = string(heading_level, '#') + " " + content + "\n\n";
	} else if (block_type == "paragraph") {
		// Plain paragraph
		result = content + "\n\n";
	} else if (block_type == "code") {
		// Fenced code block
		string language = GetAttribute(attributes, "language");
		result = "```" + language + "\n" + content + "\n```\n\n";
	} else if (block_type == "blockquote") {
		// Block quote - add > prefix to each line
		string quoted;
		std::istringstream iss(content);
		std::string line;
		while (std::getline(iss, line)) {
			quoted += "> " + line + "\n";
		}
		result = quoted + "\n";
	} else if (block_type == "list") {
		// List - content is JSON encoded
		if (encoding == "json" && content.length() > 2 && content[0] == '[') {
			bool ordered = GetAttribute(attributes, "ordered") == "true";
			int start = 1;
			string start_str = GetAttribute(attributes, "start");
			if (!start_str.empty()) {
				try {
					start = std::stoi(start_str);
				} catch (...) {
				}
			}

			auto items = ParseJsonListItems(content);
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
			result = content + "\n\n";
		}
	} else if (block_type == "table") {
		// Table - content is JSON encoded
		if (encoding == "json" && content.find("\"headers\"") != string::npos) {
			vector<string> headers;
			vector<vector<string>> rows;
			ParseJsonTable(content, headers, rows);

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
	} else if (block_type == "hr") {
		result = "---\n\n";
	} else if (block_type == "image") {
		// Image: ![alt](src "title")
		string src = GetAttribute(attributes, "src");
		string alt = GetAttribute(attributes, "alt");
		// Fall back to content as alt text if alt attribute is empty
		if (alt.empty() && !content.empty()) {
			alt = content;
		}
		string title = GetAttribute(attributes, "title");
		result = "![" + alt + "](" + src;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")\n\n";
	} else if (block_type == "raw" || block_type == "html" || block_type == "md:html_block") {
		// Raw content - output as-is
		result = content + "\n\n";
	} else {
		// Unknown block type - output content as paragraph
		result = content + "\n\n";
	}

	return result;
}

//===--------------------------------------------------------------------===//
// duck_block_to_md - Single block to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterBlockToMdFunction(ExtensionLoader &loader) {
	auto doc_block_type = MarkdownTypes::MarkdownDocBlockType();
	auto markdown_type = MarkdownTypes::MarkdownType();

	// duck_block_to_md(block) -> MARKDOWN
	ScalarFunction duck_block_to_md(
	    "duck_block_to_md", {doc_block_type}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &block_vector = args.data[0];

		    for (idx_t i = 0; i < args.size(); i++) {
			    auto block_value = block_vector.GetValue(i);

			    if (block_value.IsNull()) {
				    result.SetValue(i, Value());
				    continue;
			    }

			    // Extract fields from the struct
			    auto &struct_children = StructValue::GetChildren(block_value);
			    if (struct_children.size() < 6) {
				    result.SetValue(i, Value(""));
				    continue;
			    }

			    string block_type = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
			    string content = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
			    int32_t level = struct_children[2].IsNull() ? -1 : struct_children[2].GetValue<int32_t>();
			    string encoding = struct_children[3].IsNull() ? "text" : struct_children[3].ToString();
			    Value attributes = struct_children[4];

			    string markdown = RenderBlockToMarkdown(block_type, content, level, encoding, attributes);
			    result.SetValue(i, Value(markdown));
		    }
	    });

	loader.RegisterFunction(duck_block_to_md);
}

//===--------------------------------------------------------------------===//
// duck_blocks_to_md - List of blocks to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterBlocksToMdFunction(ExtensionLoader &loader) {
	auto doc_block_type = MarkdownTypes::MarkdownDocBlockType();
	auto doc_block_list_type = LogicalType::LIST(doc_block_type);
	auto markdown_type = MarkdownTypes::MarkdownType();

	// duck_blocks_to_md(blocks LIST) -> MARKDOWN
	ScalarFunction duck_blocks_to_md(
	    "duck_blocks_to_md", {doc_block_list_type}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &blocks_vector = args.data[0];

		    for (idx_t i = 0; i < args.size(); i++) {
			    auto blocks_value = blocks_vector.GetValue(i);

			    if (blocks_value.IsNull()) {
				    result.SetValue(i, Value());
				    continue;
			    }

			    auto &list_children = ListValue::GetChildren(blocks_value);
			    string markdown;

			    for (const auto &block_value : list_children) {
				    if (block_value.IsNull()) {
					    continue;
				    }

				    auto &struct_children = StructValue::GetChildren(block_value);
				    if (struct_children.size() < 6) {
					    continue;
				    }

				    string block_type = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
				    string content = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
				    int32_t level = struct_children[2].IsNull() ? -1 : struct_children[2].GetValue<int32_t>();
				    string encoding = struct_children[3].IsNull() ? "text" : struct_children[3].ToString();
				    Value attributes = struct_children[4];

				    markdown += RenderBlockToMarkdown(block_type, content, level, encoding, attributes);
			    }

			    result.SetValue(i, Value(markdown));
		    }
	    });

	loader.RegisterFunction(duck_blocks_to_md);
}

//===--------------------------------------------------------------------===//
// duck_blocks_to_sections - Convert blocks to sections format
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterBlocksToSectionsFunction(ExtensionLoader &loader) {
	auto doc_block_type = MarkdownTypes::MarkdownDocBlockType();
	auto doc_block_list_type = LogicalType::LIST(doc_block_type);
	auto markdown_type = MarkdownTypes::MarkdownType();

	// Section struct type: (section_id, level, title, content)
	child_list_t<LogicalType> section_struct_types;
	section_struct_types.push_back(std::make_pair("section_id", LogicalType::VARCHAR));
	section_struct_types.push_back(std::make_pair("section_path", LogicalType::VARCHAR));
	section_struct_types.push_back(std::make_pair("level", LogicalType::INTEGER));
	section_struct_types.push_back(std::make_pair("title", LogicalType::VARCHAR));
	section_struct_types.push_back(std::make_pair("content", markdown_type));

	auto section_struct_type = LogicalType::STRUCT(section_struct_types);
	auto section_list_type = LogicalType::LIST(section_struct_type);

	// duck_blocks_to_sections(blocks LIST) -> LIST(section)
	ScalarFunction duck_blocks_to_sections(
	    "duck_blocks_to_sections", {doc_block_list_type}, section_list_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &blocks_vector = args.data[0];

		    for (idx_t row_idx = 0; row_idx < args.size(); row_idx++) {
			    auto blocks_value = blocks_vector.GetValue(row_idx);

			    if (blocks_value.IsNull()) {
				    result.SetValue(row_idx, Value());
				    continue;
			    }

			    auto &list_children = ListValue::GetChildren(blocks_value);
			    vector<Value> sections;

			    // Track current section
			    string current_title;
			    int32_t current_level = 0;
			    string current_section_id;
			    string current_content;
			    vector<string> section_path_parts;

			    auto flush_section = [&]() {
				    if (!current_title.empty() || !current_content.empty()) {
					    // Build section path
					    string section_path;
					    for (size_t i = 0; i < section_path_parts.size(); i++) {
						    if (i > 0)
							    section_path += " > ";
						    section_path += section_path_parts[i];
					    }

					    child_list_t<Value> section_values;
					    section_values.push_back(std::make_pair("section_id", Value(current_section_id)));
					    section_values.push_back(std::make_pair("section_path", Value(section_path)));
					    section_values.push_back(std::make_pair("level", Value::INTEGER(current_level)));
					    section_values.push_back(std::make_pair("title", Value(current_title)));
					    section_values.push_back(std::make_pair("content", Value(current_content)));

					    sections.push_back(Value::STRUCT(section_values));
				    }
			    };

			    for (const auto &block_value : list_children) {
				    if (block_value.IsNull()) {
					    continue;
				    }

				    auto &struct_children = StructValue::GetChildren(block_value);
				    if (struct_children.size() < 6) {
					    continue;
				    }

				    string block_type = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
				    string content = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
				    int32_t level = struct_children[2].IsNull() ? 0 : struct_children[2].GetValue<int32_t>();
				    string encoding = struct_children[3].IsNull() ? "text" : struct_children[3].ToString();
				    Value attributes = struct_children[4];

				    if (block_type == "heading") {
					    // Flush previous section
					    flush_section();

					    // Update section path for new heading
					    while (section_path_parts.size() >= (size_t)level) {
						    section_path_parts.pop_back();
					    }
					    section_path_parts.push_back(content);

					    // Start new section
					    current_title = content;
					    current_level = level;
					    current_section_id = GetAttribute(attributes, "id");
					    if (current_section_id.empty()) {
						    // Generate ID from title
						    current_section_id = content;
						    std::transform(current_section_id.begin(), current_section_id.end(),
						                   current_section_id.begin(), ::tolower);
						    std::replace(current_section_id.begin(), current_section_id.end(), ' ', '-');
					    }
					    current_content.clear();
				    } else if (block_type == "metadata" || block_type == "frontmatter") {
					    // Metadata becomes level 0 section
					    flush_section();
					    current_title = "";
					    current_level = 0;
					    current_section_id = "frontmatter";
					    current_content = content;
					    flush_section();
					    current_title.clear();
					    current_content.clear();
				    } else {
					    // Append rendered content to current section
					    current_content +=
					        DuckBlockFunctions::RenderBlockToMarkdown(block_type, content, level, encoding, attributes);
				    }
			    }

			    // Flush final section
			    flush_section();

			    result.SetValue(row_idx, Value::LIST(LogicalType::STRUCT(child_list_t<LogicalType> {
			                                             {"section_id", LogicalType::VARCHAR},
			                                             {"section_path", LogicalType::VARCHAR},
			                                             {"level", LogicalType::INTEGER},
			                                             {"title", LogicalType::VARCHAR},
			                                             {"content", MarkdownTypes::MarkdownType()}}),
			                                         sections));
		    }
	    });

	loader.RegisterFunction(duck_blocks_to_sections);
}

//===--------------------------------------------------------------------===//
// Register All Functions
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::Register(ExtensionLoader &loader) {
	RegisterBlockToMdFunction(loader);
	RegisterBlocksToMdFunction(loader);
	RegisterBlocksToSectionsFunction(loader);
}

} // namespace duckdb
