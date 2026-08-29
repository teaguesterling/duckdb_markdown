#include "duck_block_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"
#include <sstream>

namespace duckdb {

using namespace duckdb_yyjson;

// Maximum Pandoc inline-nesting depth walked by ExtractPandocText. Guards
// against unbounded recursion (stack overflow) on adversarially nested JSON.
static constexpr int MAX_PANDOC_DEPTH = 1000;

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
	if (content.empty()) {
		return items;
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return items;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (yyjson_is_arr(root)) {
		size_t idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(root, idx, max, val) {
			if (yyjson_is_str(val)) {
				items.emplace_back(yyjson_get_str(val), yyjson_get_len(val));
			}
		}
	}
	yyjson_doc_free(doc);
	return items;
}

void DuckBlockFunctions::ParseJsonTable(const string &content, vector<string> &headers, vector<vector<string>> &rows) {
	if (content.empty()) {
		return;
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (yyjson_is_obj(root)) {
		yyjson_val *headers_val = yyjson_obj_get(root, "headers");
		if (headers_val && yyjson_is_arr(headers_val)) {
			size_t idx, max;
			yyjson_val *val;
			yyjson_arr_foreach(headers_val, idx, max, val) {
				if (yyjson_is_str(val)) {
					headers.emplace_back(yyjson_get_str(val), yyjson_get_len(val));
				}
			}
		}

		yyjson_val *rows_val = yyjson_obj_get(root, "rows");
		if (rows_val && yyjson_is_arr(rows_val)) {
			size_t r_idx, r_max;
			yyjson_val *row_val;
			yyjson_arr_foreach(rows_val, r_idx, r_max, row_val) {
				if (yyjson_is_arr(row_val)) {
					vector<string> row;
					size_t c_idx, c_max;
					yyjson_val *cell_val;
					yyjson_arr_foreach(row_val, c_idx, c_max, cell_val) {
						if (yyjson_is_str(cell_val)) {
							row.emplace_back(yyjson_get_str(cell_val), yyjson_get_len(cell_val));
						}
					}
					if (!row.empty()) {
						rows.push_back(std::move(row));
					}
				}
			}
		}
	}
	yyjson_doc_free(doc);
}

static string ExtractPandocTextFromVal(yyjson_val *val, int depth) {
	if (!val) {
		return "";
	}
	if (depth > MAX_PANDOC_DEPTH) {
		throw InvalidInputException("Pandoc inline nesting exceeds maximum supported depth (%d)", MAX_PANDOC_DEPTH);
	}
	if (yyjson_is_str(val)) {
		return string(yyjson_get_str(val), yyjson_get_len(val));
	}
	if (yyjson_is_arr(val)) {
		string result;
		size_t idx, max;
		yyjson_val *item;
		yyjson_arr_foreach(val, idx, max, item) {
			result += ExtractPandocTextFromVal(item, depth);
		}
		return result;
	}
	if (yyjson_is_obj(val)) {
		yyjson_val *t_val = yyjson_obj_get(val, "t");
		if (!t_val || !yyjson_is_str(t_val)) {
			return "";
		}
		const char *t = yyjson_get_str(t_val);
		yyjson_val *c_val = yyjson_obj_get(val, "c");

		if (strcmp(t, "Str") == 0) {
			if (c_val && yyjson_is_str(c_val)) {
				return string(yyjson_get_str(c_val), yyjson_get_len(c_val));
			}
		} else if (strcmp(t, "Space") == 0 || strcmp(t, "SoftBreak") == 0) {
			return " ";
		} else if (strcmp(t, "LineBreak") == 0) {
			return "\n";
		} else if (strcmp(t, "Strong") == 0) {
			return "**" + ExtractPandocTextFromVal(c_val, depth + 1) + "**";
		} else if (strcmp(t, "Emph") == 0) {
			return "*" + ExtractPandocTextFromVal(c_val, depth + 1) + "*";
		} else if (strcmp(t, "Strikeout") == 0) {
			return "~~" + ExtractPandocTextFromVal(c_val, depth + 1) + "~~";
		} else if (strcmp(t, "Superscript") == 0) {
			return "^" + ExtractPandocTextFromVal(c_val, depth + 1) + "^";
		} else if (strcmp(t, "Subscript") == 0) {
			return "~" + ExtractPandocTextFromVal(c_val, depth + 1) + "~";
		} else if (strcmp(t, "Code") == 0) {
			if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
				yyjson_val *code_str = yyjson_arr_get(c_val, 1);
				if (code_str && yyjson_is_str(code_str)) {
					return "`" + string(yyjson_get_str(code_str), yyjson_get_len(code_str)) + "`";
				}
			}
		} else if (strcmp(t, "Math") == 0) {
			if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 2) {
				yyjson_val *math_str = yyjson_arr_get(c_val, 1);
				if (math_str && yyjson_is_str(math_str)) {
					return "$" + string(yyjson_get_str(math_str), yyjson_get_len(math_str)) + "$";
				}
			}
		} else if (strcmp(t, "Link") == 0) {
			if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 3) {
				yyjson_val *inlines = yyjson_arr_get(c_val, 1);
				yyjson_val *target = yyjson_arr_get(c_val, 2);
				string text = ExtractPandocTextFromVal(inlines, depth + 1);
				string url;
				if (target && yyjson_is_arr(target) && yyjson_arr_size(target) >= 1) {
					yyjson_val *u = yyjson_arr_get(target, 0);
					if (u && yyjson_is_str(u)) {
						url = string(yyjson_get_str(u), yyjson_get_len(u));
					}
				}
				return "[" + text + "](" + url + ")";
			}
		} else if (strcmp(t, "Image") == 0) {
			if (c_val && yyjson_is_arr(c_val) && yyjson_arr_size(c_val) >= 3) {
				yyjson_val *inlines = yyjson_arr_get(c_val, 1);
				yyjson_val *target = yyjson_arr_get(c_val, 2);
				string alt = ExtractPandocTextFromVal(inlines, depth + 1);
				string src;
				if (target && yyjson_is_arr(target) && yyjson_arr_size(target) >= 1) {
					yyjson_val *s = yyjson_arr_get(target, 0);
					if (s && yyjson_is_str(s)) {
						src = string(yyjson_get_str(s), yyjson_get_len(s));
					}
				}
				return "![" + alt + "](" + src + ")";
			}
		} else if (strcmp(t, "Plain") == 0 || strcmp(t, "Para") == 0 || strcmp(t, "Span") == 0 ||
		           strcmp(t, "SmallCaps") == 0 || strcmp(t, "Quoted") == 0 || strcmp(t, "Cite") == 0) {
			return ExtractPandocTextFromVal(c_val, depth + 1);
		}
	}
	return "";
}

string DuckBlockFunctions::ExtractPandocText(const string &content, int depth) {
	if (content.empty()) {
		return "";
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return "";
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	string result = ExtractPandocTextFromVal(root, depth);
	yyjson_doc_free(doc);
	return result;
}

bool DuckBlockFunctions::IsPandocTableFormat(const string &content) {
	if (content.empty()) {
		return false;
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	bool is_table = false;
	if (yyjson_is_arr(root)) {
		size_t sz = yyjson_arr_size(root);
		if (sz == 5 || sz == 6) {
			yyjson_val *e1 = yyjson_arr_get(root, 1);
			if (e1 && yyjson_is_arr(e1) && yyjson_arr_size(e1) > 0) {
				yyjson_val *first = yyjson_arr_get(e1, 0);
				if (first && yyjson_is_obj(first)) {
					yyjson_val *t = yyjson_obj_get(first, "t");
					if (t && yyjson_is_str(t) && strncmp(yyjson_get_str(t), "Align", 5) == 0) {
						is_table = true;
					}
				}
			}
			if (!is_table && sz == 6) {
				yyjson_val *e2 = yyjson_arr_get(root, 2);
				if (e2 && yyjson_is_arr(e2)) {
					is_table = true;
				}
			}
		}
	}
	yyjson_doc_free(doc);
	return is_table;
}

static string ExtractPandocCellText(yyjson_val *cell_val) {
	if (!cell_val) {
		return "";
	}
	if (yyjson_is_arr(cell_val)) {
		string text;
		size_t idx, max;
		yyjson_val *block;
		yyjson_arr_foreach(cell_val, idx, max, block) {
			if (yyjson_is_obj(block)) {
				yyjson_val *c = yyjson_obj_get(block, "c");
				if (c) {
					text += ExtractPandocTextFromVal(c, 0);
				}
			}
		}
		return text;
	}
	return ExtractPandocTextFromVal(cell_val, 0);
}

void DuckBlockFunctions::ParsePandocTable(const string &content, vector<string> &headers,
                                          vector<vector<string>> &rows) {
	if (content.empty()) {
		return;
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (!yyjson_is_arr(root)) {
		yyjson_doc_free(doc);
		return;
	}

	size_t root_size = yyjson_arr_size(root);

	if (root_size == 5) {
		// Legacy format: [caption, alignments, widths, headers, rows]
		yyjson_val *headers_arr = yyjson_arr_get(root, 3);
		if (headers_arr && yyjson_is_arr(headers_arr)) {
			size_t idx, max;
			yyjson_val *cell;
			yyjson_arr_foreach(headers_arr, idx, max, cell) {
				headers.push_back(ExtractPandocCellText(cell));
			}
		}

		yyjson_val *rows_arr = yyjson_arr_get(root, 4);
		if (rows_arr && yyjson_is_arr(rows_arr)) {
			size_t r_idx, r_max;
			yyjson_val *row;
			yyjson_arr_foreach(rows_arr, r_idx, r_max, row) {
				if (yyjson_is_arr(row)) {
					vector<string> r;
					size_t c_idx, c_max;
					yyjson_val *cell;
					yyjson_arr_foreach(row, c_idx, c_max, cell) {
						r.push_back(ExtractPandocCellText(cell));
					}
					if (!r.empty()) {
						rows.push_back(std::move(r));
					}
				}
			}
		}
	} else if (root_size >= 6) {
		// Modern format: [attr, caption, colspec, tableHead, tableBodies, tableFoot]
		yyjson_val *table_head = yyjson_arr_get(root, 3);
		if (table_head && yyjson_is_arr(table_head) && yyjson_arr_size(table_head) >= 2) {
			yyjson_val *head_rows = yyjson_arr_get(table_head, 1);
			if (head_rows && yyjson_is_arr(head_rows)) {
				size_t hr_idx, hr_max;
				yyjson_val *head_row;
				yyjson_arr_foreach(head_rows, hr_idx, hr_max, head_row) {
					if (yyjson_is_arr(head_row) && yyjson_arr_size(head_row) >= 2) {
						yyjson_val *cells = yyjson_arr_get(head_row, 1);
						if (cells && yyjson_is_arr(cells)) {
							size_t c_idx, c_max;
							yyjson_val *cell;
							yyjson_arr_foreach(cells, c_idx, c_max, cell) {
								if (yyjson_is_arr(cell) && yyjson_arr_size(cell) >= 5) {
									yyjson_val *blocks = yyjson_arr_get(cell, 4);
									headers.push_back(ExtractPandocCellText(blocks));
								} else {
									headers.push_back(ExtractPandocCellText(cell));
								}
							}
						}
					}
				}
			}
		}

		yyjson_val *table_bodies = yyjson_arr_get(root, 4);
		if (table_bodies && yyjson_is_arr(table_bodies)) {
			size_t b_idx, b_max;
			yyjson_val *body;
			yyjson_arr_foreach(table_bodies, b_idx, b_max, body) {
				if (yyjson_is_arr(body) && yyjson_arr_size(body) >= 4) {
					yyjson_val *body_rows = yyjson_arr_get(body, 3);
					if (body_rows && yyjson_is_arr(body_rows)) {
						size_t r_idx, r_max;
						yyjson_val *row;
						yyjson_arr_foreach(body_rows, r_idx, r_max, row) {
							if (yyjson_is_arr(row) && yyjson_arr_size(row) >= 2) {
								yyjson_val *cells = yyjson_arr_get(row, 1);
								if (cells && yyjson_is_arr(cells)) {
									vector<string> r;
									size_t c_idx, c_max;
									yyjson_val *cell;
									yyjson_arr_foreach(cells, c_idx, c_max, cell) {
										if (yyjson_is_arr(cell) && yyjson_arr_size(cell) >= 5) {
											yyjson_val *blocks = yyjson_arr_get(cell, 4);
											r.push_back(ExtractPandocCellText(blocks));
										} else {
											r.push_back(ExtractPandocCellText(cell));
										}
									}
									if (!r.empty()) {
										rows.push_back(std::move(r));
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (headers.empty() && !rows.empty()) {
		headers = rows[0];
		rows.erase(rows.begin());
	}

	yyjson_doc_free(doc);
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
	} else if (element_type == "smallcaps") {
		// No standard markdown for small caps, use span with style
		return "<span style=\"font-variant: small-caps\">" + content + "</span>";
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
// RenderBlockElementToMarkdown (helper for block elements)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderBlockElementToMarkdown(const string &element_type, const string &content,
                                                        int32_t level, const string &encoding,
                                                        const Value &attributes) {
	string result;

	if (element_type == "frontmatter" || element_type == "metadata") {
		// YAML frontmatter
		result = "---\n" + content + "\n---\n\n";
	} else if (element_type == "heading") {
		// ATX heading with level
		// Per spec: heading_level attribute takes priority, fall back to level field
		int32_t heading_level = 1;
		string heading_level_attr = GetAttribute(attributes, "heading_level");
		if (!heading_level_attr.empty()) {
			try {
				heading_level = std::stoi(heading_level_attr);
			} catch (...) {
				heading_level = 1;
			}
		} else if (level > 0 && level <= 6) {
			heading_level = level;
		}
		// Clamp to valid range
		if (heading_level < 1)
			heading_level = 1;
		if (heading_level > 6)
			heading_level = 6;
		result = string(heading_level, '#') + " " + content + "\n\n";
	} else if (element_type == "paragraph") {
		// Plain paragraph
		result = content + "\n\n";
	} else if (element_type == "code") {
		// Fenced code block
		string language = GetAttribute(attributes, "language");
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
	} else if (element_type == "table") {
		// Table - content is JSON encoded
		vector<string> headers;
		vector<vector<string>> rows;
		bool parsed = false;

		if (encoding == "json") {
			// Try standard {"headers": [...], "rows": [...]} format first
			if (content.find("\"headers\"") != string::npos) {
				ParseJsonTable(content, headers, rows);
				parsed = !headers.empty();
			}
			// Try Pandoc table format if standard format didn't work
			if (!parsed && IsPandocTableFormat(content)) {
				ParsePandocTable(content, headers, rows);
				parsed = !headers.empty() || !rows.empty();
			}
		}

		if (parsed && (!headers.empty() || !rows.empty())) {
			// Determine column count
			size_t col_count = headers.size();
			if (col_count == 0 && !rows.empty()) {
				col_count = rows[0].size();
			}

			// Render headers (or empty header row if we only have body rows)
			result = "|";
			if (!headers.empty()) {
				for (const auto &h : headers) {
					result += " " + h + " |";
				}
			} else {
				for (size_t i = 0; i < col_count; i++) {
					result += " |";
				}
			}
			result += "\n|";
			for (size_t i = 0; i < col_count; i++) {
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
	} else if (element_type == "hr") {
		result = "---\n\n";
	} else if (element_type == "list_item") {
		// List item - render with bullet prefix
		// Check if ordered from attributes
		bool ordered = GetAttribute(attributes, "ordered") == "true";
		string item_num = GetAttribute(attributes, "item_number");
		if (ordered && !item_num.empty()) {
			result = item_num + ". " + content + "\n";
		} else {
			result = "- " + content + "\n";
		}
	} else if (element_type == "image") {
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
	} else if (element_type == "raw" || element_type == "html" || element_type == "md:html_block") {
		// Raw content - output as-is
		result = content + "\n\n";
	} else {
		// Unknown block type - output content as paragraph
		result = content + "\n\n";
	}

	return result;
}

//===--------------------------------------------------------------------===//
// RenderDuckBlockToMarkdown (unified block/inline rendering)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderDuckBlockToMarkdown(const string &kind, const string &element_type,
                                                     const string &content, int32_t level, const string &encoding,
                                                     const Value &attributes) {
	if (kind == "block") {
		// Delegate to block rendering
		return RenderBlockElementToMarkdown(element_type, content, level, encoding, attributes);
	} else if (kind == "inline") {
		// Use inline rendering
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	} else {
		// Unknown kind - try to guess based on element_type
		// Block types
		if (element_type == "heading" || element_type == "paragraph" || element_type == "blockquote" ||
		    element_type == "list" || element_type == "table" || element_type == "hr" || element_type == "metadata" ||
		    element_type == "frontmatter" || element_type == "code" || element_type == "image") {
			return RenderBlockElementToMarkdown(element_type, content, level, encoding, attributes);
		}
		// Assume inline otherwise
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	}
}

//===--------------------------------------------------------------------===//
// RenderDuckBlocksToMarkdown (list of duck_blocks)
//===--------------------------------------------------------------------===//

string DuckBlockFunctions::RenderDuckBlocksToMarkdown(const Value &blocks_value) {
	if (blocks_value.IsNull() || blocks_value.type().id() != LogicalTypeId::LIST) {
		return "";
	}

	auto &list_children = ListValue::GetChildren(blocks_value);
	string result;
	bool last_was_inline = false;

	for (const auto &block_value : list_children) {
		if (block_value.IsNull()) {
			continue;
		}

		auto &struct_children = StructValue::GetChildren(block_value);
		if (struct_children.size() < 7) {
			continue;
		}

		// duck_block: kind, element_type, content, level, encoding, attributes, element_order
		string kind = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
		string element_type = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
		string content = struct_children[2].IsNull() ? "" : struct_children[2].ToString();
		int32_t level = struct_children[3].IsNull() ? 0 : struct_children[3].GetValue<int32_t>();
		string encoding = struct_children[4].IsNull() ? "text" : struct_children[4].ToString();
		Value attributes = struct_children[5];

		bool is_inline = (kind == "inline");

		// Handle inline-to-block transition: add paragraph break
		if (last_was_inline && !is_inline) {
			result += "\n\n";
		}

		result += RenderDuckBlockToMarkdown(kind, element_type, content, level, encoding, attributes);
		last_was_inline = is_inline;
	}

	return result;
}

//===--------------------------------------------------------------------===//
// duck_block_to_md - Single duck_block to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterDuckBlockToMdFunction(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto markdown_type = MarkdownTypes::MarkdownType();

	// duck_block_to_md(block) -> MARKDOWN
	ScalarFunction duck_block_to_md(
	    "duck_block_to_md", {duck_block_type}, markdown_type,
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
			    if (struct_children.size() < 7) {
				    result.SetValue(i, Value(""));
				    continue;
			    }

			    // duck_block: kind, element_type, content, level, encoding, attributes, element_order
			    string kind = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
			    string element_type = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
			    string content = struct_children[2].IsNull() ? "" : struct_children[2].ToString();
			    int32_t level = struct_children[3].IsNull() ? 0 : struct_children[3].GetValue<int32_t>();
			    string encoding = struct_children[4].IsNull() ? "text" : struct_children[4].ToString();
			    Value attributes = struct_children[5];

			    string markdown = RenderDuckBlockToMarkdown(kind, element_type, content, level, encoding, attributes);
			    result.SetValue(i, Value(markdown));
		    }
	    });

	loader.RegisterFunction(duck_block_to_md);
}

//===--------------------------------------------------------------------===//
// duck_blocks_to_md - List of duck_blocks to Markdown
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterDuckBlocksToMdFunction(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto duck_block_list_type = LogicalType::LIST(duck_block_type);
	auto markdown_type = MarkdownTypes::MarkdownType();

	// duck_blocks_to_md(blocks LIST) -> MARKDOWN
	ScalarFunction duck_blocks_to_md("duck_blocks_to_md", {duck_block_list_type}, markdown_type,
	                                 [](DataChunk &args, ExpressionState &state, Vector &result) {
		                                 auto &blocks_vector = args.data[0];

		                                 for (idx_t i = 0; i < args.size(); i++) {
			                                 auto blocks_value = blocks_vector.GetValue(i);

			                                 if (blocks_value.IsNull()) {
				                                 result.SetValue(i, Value());
				                                 continue;
			                                 }

			                                 string markdown = RenderDuckBlocksToMarkdown(blocks_value);
			                                 result.SetValue(i, Value(markdown));
		                                 }
	                                 });

	loader.RegisterFunction(duck_blocks_to_md);
}

//===--------------------------------------------------------------------===//
// duck_blocks_to_sections - Convert duck_blocks to sections format
//===--------------------------------------------------------------------===//

void DuckBlockFunctions::RegisterDuckBlocksToSectionsFunction(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto duck_block_list_type = LogicalType::LIST(duck_block_type);
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
	    "duck_blocks_to_sections", {duck_block_list_type}, section_list_type,
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
				    if (struct_children.size() < 7) {
					    continue;
				    }

				    // duck_block: kind, element_type, content, level, encoding, attributes, element_order
				    string kind = struct_children[0].IsNull() ? "" : struct_children[0].ToString();
				    string element_type = struct_children[1].IsNull() ? "" : struct_children[1].ToString();
				    string content = struct_children[2].IsNull() ? "" : struct_children[2].ToString();
				    int32_t level = struct_children[3].IsNull() ? 0 : struct_children[3].GetValue<int32_t>();
				    string encoding = struct_children[4].IsNull() ? "text" : struct_children[4].ToString();
				    Value attributes = struct_children[5];

				    if (element_type == "heading") {
					    // Flush previous section
					    flush_section();

					    // Get heading level: attribute takes priority, fall back to level field
					    int32_t heading_level = 1;
					    string heading_level_attr = GetAttribute(attributes, "heading_level");
					    if (!heading_level_attr.empty()) {
						    try {
							    heading_level = std::stoi(heading_level_attr);
						    } catch (...) {
							    heading_level = 1;
						    }
					    } else if (level > 0 && level <= 6) {
						    heading_level = level;
					    }
					    // Clamp to valid range
					    if (heading_level < 1)
						    heading_level = 1;
					    if (heading_level > 6)
						    heading_level = 6;

					    // Update section path for new heading
					    while (section_path_parts.size() >= (size_t)heading_level) {
						    section_path_parts.pop_back();
					    }
					    section_path_parts.push_back(content);

					    // Start new section
					    current_title = content;
					    current_level = heading_level;
					    current_section_id = GetAttribute(attributes, "id");
					    if (current_section_id.empty()) {
						    // Generate ID from title
						    // ASCII-only lowercase: ::tolower on a raw (signed) char is UB for
						    // any UTF-8 byte >= 0x80, and this id keeps its non-ASCII bytes.
						    current_section_id = StringUtil::Lower(content);
						    std::replace(current_section_id.begin(), current_section_id.end(), ' ', '-');
					    }
					    current_content.clear();
				    } else if (element_type == "metadata" || element_type == "frontmatter") {
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
					        RenderDuckBlockToMarkdown(kind, element_type, content, level, encoding, attributes);
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

// Build a duck_block STRUCT Value from a parsed MarkdownBlock. Empty content
// becomes NULL (the spec's convention for containers with structured children).
static Value MakeDuckBlockValue(const markdown_utils::MarkdownBlock &b) {
	child_list_t<Value> fields;
	fields.emplace_back("kind", Value(b.kind));
	fields.emplace_back("element_type", Value(b.block_type));
	fields.emplace_back("content", b.content.empty() ? Value(LogicalType::VARCHAR) : Value(b.content));
	fields.emplace_back("level", b.level >= 0 ? Value::INTEGER(b.level) : Value(LogicalType::INTEGER));
	fields.emplace_back("encoding", Value(b.encoding));
	vector<Value> keys, vals;
	for (const auto &a : b.attributes) {
		keys.push_back(Value(a.first));
		vals.push_back(Value(a.second));
	}
	fields.emplace_back("attributes", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, vals));
	fields.emplace_back("element_order", Value::INTEGER(b.block_order));
	return Value::STRUCT(fields);
}

// parse_markdown_to_duck_blocks(md VARCHAR) -> LIST(duck_block)
// Scalar counterpart of read_markdown_blocks that emits the canonical structured
// representation: rich text as kind='inline' children, not markdown-in-content.
static void RegisterParseMarkdownToDuckBlocks(ExtensionLoader &loader) {
	auto duck_block_type = MarkdownTypes::DuckBlockType();
	auto duck_block_list_type = LogicalType::LIST(duck_block_type);

	ScalarFunction fn("parse_markdown_to_duck_blocks", {LogicalType::VARCHAR}, duck_block_list_type,
	                  [](DataChunk &args, ExpressionState &state, Vector &result) {
		                  auto &in = args.data[0];
		                  auto dtype = MarkdownTypes::DuckBlockType();
		                  for (idx_t i = 0; i < args.size(); i++) {
			                  auto v = in.GetValue(i);
			                  if (v.IsNull()) {
				                  result.SetValue(i, Value());
				                  continue;
			                  }
			                  auto blocks = markdown_utils::ParseBlocks(v.ToString(), /*structured_inlines=*/true);
			                  vector<Value> vals;
			                  vals.reserve(blocks.size());
			                  for (const auto &b : blocks) {
				                  vals.push_back(MakeDuckBlockValue(b));
			                  }
			                  result.SetValue(i, Value::LIST(dtype, vals));
		                  }
	                  });
	loader.RegisterFunction(fn);
}

void DuckBlockFunctions::Register(ExtensionLoader &loader) {
	RegisterDuckBlockToMdFunction(loader);
	RegisterDuckBlocksToMdFunction(loader);
	RegisterDuckBlocksToSectionsFunction(loader);
	RegisterParseMarkdownToDuckBlocks(loader);
}

} // namespace duckdb
