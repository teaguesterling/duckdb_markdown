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
// RenderInlineElementToMarkdown (helper for inline elements)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderInlineElementToMarkdown(const string &element_type, const string &content,
                                                          const Value &attributes) {
	if (element_type == "link") {
		// [text](href "title")
		string href = GetAttribute(attributes, "href");
		string title = GetAttribute(attributes, "title");
		string result = "[" + content + "](" + href;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")";
		return result;
	} else if (element_type == "image") {
		// ![alt](src "title")
		string src = GetAttribute(attributes, "src");
		string title = GetAttribute(attributes, "title");
		string result = "![" + content + "](" + src;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")";
		return result;
	} else if (element_type == "bold" || element_type == "strong") {
		// **text**
		return "**" + content + "**";
	} else if (element_type == "italic" || element_type == "emphasis" || element_type == "em") {
		// *text*
		return "*" + content + "*";
	} else if (element_type == "code") {
		// `text` - inline code
		// Handle content containing backticks
		if (content.find('`') != string::npos) {
			return "`` " + content + " ``";
		}
		return "`" + content + "`";
	} else if (element_type == "text") {
		// Plain text
		return content;
	} else if (element_type == "space") {
		// Word separator
		return " ";
	} else if (element_type == "softbreak") {
		// Soft line break
		return "\n";
	} else if (element_type == "linebreak" || element_type == "br") {
		// Hard line break
		return "  \n";
	} else if (element_type == "strikethrough" || element_type == "del") {
		// ~~text~~
		return "~~" + content + "~~";
	} else if (element_type == "superscript" || element_type == "sup") {
		// ^text^
		return "^" + content + "^";
	} else if (element_type == "subscript" || element_type == "sub") {
		// ~text~
		return "~" + content + "~";
	} else if (element_type == "underline") {
		// <u>text</u> (no standard markdown, use HTML)
		return "<u>" + content + "</u>";
	} else if (element_type == "math") {
		// $text$ for inline, $$text$$ for display
		string display = GetAttribute(attributes, "display");
		if (display == "block") {
			return "$$" + content + "$$";
		}
		return "$" + content + "$";
	} else if (element_type == "raw") {
		// Raw content as-is
		return content;
	} else if (element_type == "quoted") {
		// Quoted text
		string quote_type = GetAttribute(attributes, "quote_type");
		if (quote_type == "single") {
			return "'" + content + "'";
		}
		return "\"" + content + "\"";
	} else if (element_type == "cite") {
		// Citation [@key]
		string key = GetAttribute(attributes, "key");
		if (!key.empty()) {
			return "[@" + key + "]";
		}
		return content;
	} else if (element_type == "note") {
		// Footnote [^note]
		return "[^" + content + "]";
	} else if (element_type == "span") {
		// Generic span - just output content
		return content;
	} else {
		// Unknown inline type - output as plain text
		return content;
	}
}

//===--------------------------------------------------------------------===//
// RenderElementToMarkdown (unified block/inline rendering)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderElementToMarkdown(const string &kind, const string &element_type,
                                                    const string &content, int32_t level,
                                                    const string &encoding, const Value &attributes) {
	if (kind == "block") {
		// Delegate to block rendering
		return RenderBlockToMarkdown(element_type, content, level, encoding, attributes);
	} else if (kind == "inline") {
		// Use inline rendering
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	} else {
		// Unknown kind - try to guess based on element_type
		// Block types
		if (element_type == "heading" || element_type == "paragraph" || element_type == "blockquote" ||
		    element_type == "list" || element_type == "table" || element_type == "hr" ||
		    element_type == "metadata" || element_type == "frontmatter") {
			return RenderBlockToMarkdown(element_type, content, level, encoding, attributes);
		}
		// Assume inline otherwise
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	}
}

//===--------------------------------------------------------------------===//
// RenderElementsToMarkdown (list of elements)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderElementsToMarkdown(const Value &elements_value) {
	if (elements_value.IsNull() || elements_value.type().id() != LogicalTypeId::LIST) {
		return "";
	}

	auto &list_children = ListValue::GetChildren(elements_value);
	string result;

	for (const auto &element_value : list_children) {
		if (element_value.IsNull()) {
			continue;
		}

		auto &struct_children = StructValue::GetChildren(element_value);
		if (struct_children.size() < 7) {
			continue;
		}

		// doc_element: kind, element_type, content, level, encoding, attributes, element_order
		string kind = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
		string element_type = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
		string content = struct_children[2].IsNull() ? "" : struct_children[2].ToString();
		int32_t level = struct_children[3].IsNull() ? 0 : struct_children[3].GetValue<int32_t>();
		string encoding = struct_children[4].IsNull() ? "text" : struct_children[4].ToString();
		Value attributes = struct_children[5];

		result += RenderElementToMarkdown(kind, element_type, content, level, encoding, attributes);
	}

	return result;
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
// doc_element_to_md - Single element to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterElementToMdFunction(ExtensionLoader &loader) {
	auto doc_element_type = MarkdownTypes::DocElementType();
	auto markdown_type = MarkdownTypes::MarkdownType();

	// doc_element_to_md(element) -> MARKDOWN
	ScalarFunction doc_element_to_md(
	    "doc_element_to_md", {doc_element_type}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &element_vector = args.data[0];

		    for (idx_t i = 0; i < args.size(); i++) {
			    auto element_value = element_vector.GetValue(i);

			    if (element_value.IsNull()) {
				    result.SetValue(i, Value());
				    continue;
			    }

			    // Extract fields from the struct
			    auto &struct_children = StructValue::GetChildren(element_value);
			    if (struct_children.size() < 7) {
				    result.SetValue(i, Value(""));
				    continue;
			    }

			    // doc_element: kind, element_type, content, level, encoding, attributes, element_order
			    string kind = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
			    string element_type = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
			    string content = struct_children[2].IsNull() ? "" : struct_children[2].ToString();
			    int32_t level = struct_children[3].IsNull() ? 0 : struct_children[3].GetValue<int32_t>();
			    string encoding = struct_children[4].IsNull() ? "text" : struct_children[4].ToString();
			    Value attributes = struct_children[5];

			    string markdown = RenderElementToMarkdown(kind, element_type, content, level, encoding, attributes);
			    result.SetValue(i, Value(markdown));
		    }
	    });

	loader.RegisterFunction(doc_element_to_md);
}

//===--------------------------------------------------------------------===//
// doc_elements_to_md - List of elements to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterElementsToMdFunction(ExtensionLoader &loader) {
	auto doc_element_type = MarkdownTypes::DocElementType();
	auto doc_element_list_type = LogicalType::LIST(doc_element_type);
	auto markdown_type = MarkdownTypes::MarkdownType();

	// doc_elements_to_md(elements LIST) -> MARKDOWN
	ScalarFunction doc_elements_to_md(
	    "doc_elements_to_md", {doc_element_list_type}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &elements_vector = args.data[0];

		    for (idx_t i = 0; i < args.size(); i++) {
			    auto elements_value = elements_vector.GetValue(i);

			    if (elements_value.IsNull()) {
				    result.SetValue(i, Value());
				    continue;
			    }

			    string markdown = RenderElementsToMarkdown(elements_value);
			    result.SetValue(i, Value(markdown));
		    }
	    });

	loader.RegisterFunction(doc_elements_to_md);
}

//===--------------------------------------------------------------------===//
// Register All Functions
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::Register(ExtensionLoader &loader) {
	RegisterBlockToMdFunction(loader);
	RegisterBlocksToMdFunction(loader);
	RegisterBlocksToSectionsFunction(loader);
	RegisterElementToMdFunction(loader);
	RegisterElementsToMdFunction(loader);
}

} // namespace duckdb
