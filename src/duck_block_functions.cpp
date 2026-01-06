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

// Helper to extract text with markdown formatting from Pandoc AST inline elements
// Handles {"t":"Str","c":"text"}, {"t":"Space"}, {"t":"Strong","c":[...]}, {"t":"Emph","c":[...]},
// {"t":"Code","c":[[attr],text]}, {"t":"Link","c":[[attr],[inlines],[url,title]]}, etc.
string DuckBlockFunctions::ExtractPandocText(const string &content) {
	string result;
	size_t pos = 0;

	// Helper lambda to extract a quoted string starting at a position
	auto extract_quoted_string = [&content](size_t start) -> pair<string, size_t> {
		if (start >= content.size() || content[start] != '"') {
			return {"", start};
		}
		size_t end = start + 1;
		bool escape = false;
		while (end < content.size()) {
			if (escape) {
				escape = false;
			} else if (content[end] == '\\') {
				escape = true;
			} else if (content[end] == '"') {
				break;
			}
			end++;
		}
		string text = content.substr(start + 1, end - start - 1);
		// Unescape
		string unescaped;
		for (size_t i = 0; i < text.size(); i++) {
			if (text[i] == '\\' && i + 1 < text.size()) {
				char next = text[i + 1];
				if (next == 'n') { unescaped += '\n'; i++; }
				else if (next == 't') { unescaped += '\t'; i++; }
				else if (next == '"') { unescaped += '"'; i++; }
				else if (next == '\\') { unescaped += '\\'; i++; }
				else { unescaped += text[i]; }
			} else {
				unescaped += text[i];
			}
		}
		return {unescaped, end + 1};
	};

	// Helper to find matching bracket end position
	auto find_bracket_end = [&content](size_t start) -> size_t {
		if (start >= content.size() || content[start] != '[') {
			return string::npos;
		}
		int depth = 0;
		bool in_string = false;
		bool escape = false;
		for (size_t i = start; i < content.size(); i++) {
			char c = content[i];
			if (escape) {
				escape = false;
				continue;
			}
			if (c == '\\') {
				escape = true;
				continue;
			}
			if (c == '"') {
				in_string = !in_string;
				continue;
			}
			if (in_string) continue;
			if (c == '[') depth++;
			else if (c == ']') {
				depth--;
				if (depth == 0) return i;
			}
		}
		return string::npos;
	};

	while (pos < content.size()) {
		// Find all possible type markers
		size_t str_pos = content.find("\"t\":\"Str\"", pos);
		size_t space_pos = content.find("\"t\":\"Space\"", pos);
		size_t softbreak_pos = content.find("\"t\":\"SoftBreak\"", pos);
		size_t strong_pos = content.find("\"t\":\"Strong\"", pos);
		size_t emph_pos = content.find("\"t\":\"Emph\"", pos);
		size_t code_pos = content.find("\"t\":\"Code\"", pos);
		size_t link_pos = content.find("\"t\":\"Link\"", pos);

		// Find the nearest match and its type
		size_t next_pos = string::npos;
		int match_type = 0; // 0=none, 1=Str, 2=Space, 3=SoftBreak, 4=Strong, 5=Emph, 6=Code, 7=Link

		if (str_pos != string::npos && (next_pos == string::npos || str_pos < next_pos)) {
			next_pos = str_pos;
			match_type = 1;
		}
		if (space_pos != string::npos && (next_pos == string::npos || space_pos < next_pos)) {
			next_pos = space_pos;
			match_type = 2;
		}
		if (softbreak_pos != string::npos && (next_pos == string::npos || softbreak_pos < next_pos)) {
			next_pos = softbreak_pos;
			match_type = 3;
		}
		if (strong_pos != string::npos && (next_pos == string::npos || strong_pos < next_pos)) {
			next_pos = strong_pos;
			match_type = 4;
		}
		if (emph_pos != string::npos && (next_pos == string::npos || emph_pos < next_pos)) {
			next_pos = emph_pos;
			match_type = 5;
		}
		if (code_pos != string::npos && (next_pos == string::npos || code_pos < next_pos)) {
			next_pos = code_pos;
			match_type = 6;
		}
		if (link_pos != string::npos && (next_pos == string::npos || link_pos < next_pos)) {
			next_pos = link_pos;
			match_type = 7;
		}

		if (match_type == 0) break;

		if (match_type == 1) {
			// Str: {"t":"Str","c":"text"}
			size_t c_pos = content.find("\"c\":", next_pos);
			if (c_pos != string::npos && c_pos < next_pos + 50) {
				size_t quote_start = content.find('"', c_pos + 4);
				if (quote_start != string::npos) {
					auto [text, end_pos] = extract_quoted_string(quote_start);
					result += text;
					pos = end_pos;
					continue;
				}
			}
		} else if (match_type == 2 || match_type == 3) {
			// Space or SoftBreak
			result += ' ';
			pos = next_pos + 12;
			continue;
		} else if (match_type == 4) {
			// Strong: {"t":"Strong","c":[...inlines...]}
			size_t c_pos = content.find("\"c\":", next_pos);
			if (c_pos != string::npos) {
				size_t arr_start = content.find('[', c_pos);
				if (arr_start != string::npos) {
					size_t arr_end = find_bracket_end(arr_start);
					if (arr_end != string::npos) {
						string inner = content.substr(arr_start, arr_end - arr_start + 1);
						result += "**" + ExtractPandocText(inner) + "**";
						pos = arr_end + 1;
						continue;
					}
				}
			}
		} else if (match_type == 5) {
			// Emph: {"t":"Emph","c":[...inlines...]}
			size_t c_pos = content.find("\"c\":", next_pos);
			if (c_pos != string::npos) {
				size_t arr_start = content.find('[', c_pos);
				if (arr_start != string::npos) {
					size_t arr_end = find_bracket_end(arr_start);
					if (arr_end != string::npos) {
						string inner = content.substr(arr_start, arr_end - arr_start + 1);
						result += "*" + ExtractPandocText(inner) + "*";
						pos = arr_end + 1;
						continue;
					}
				}
			}
		} else if (match_type == 6) {
			// Code: {"t":"Code","c":[[attr], "code text"]}
			size_t c_pos = content.find("\"c\":", next_pos);
			if (c_pos != string::npos) {
				size_t arr_start = content.find('[', c_pos);
				if (arr_start != string::npos) {
					// Skip the attr array and find the code string
					size_t inner_arr = content.find('[', arr_start + 1);
					if (inner_arr != string::npos) {
						size_t inner_end = find_bracket_end(inner_arr);
						if (inner_end != string::npos) {
							// Find the comma after the attr array, then the quote
							size_t comma = content.find(',', inner_end);
							if (comma != string::npos) {
								size_t quote = content.find('"', comma);
								if (quote != string::npos) {
									auto [code_text, end_pos] = extract_quoted_string(quote);
									result += "`" + code_text + "`";
									pos = end_pos;
									continue;
								}
							}
						}
					}
				}
			}
		} else if (match_type == 7) {
			// Link: {"t":"Link","c":[[attr],[...inlines...],["url","title"]]}
			size_t c_pos = content.find("\"c\":", next_pos);
			if (c_pos != string::npos) {
				size_t arr_start = content.find('[', c_pos);
				if (arr_start != string::npos) {
					// Skip attr array
					size_t attr_arr = content.find('[', arr_start + 1);
					if (attr_arr != string::npos) {
						size_t attr_end = find_bracket_end(attr_arr);
						if (attr_end != string::npos) {
							// Find the inlines array (second element)
							size_t inlines_arr = content.find('[', attr_end + 1);
							if (inlines_arr != string::npos) {
								size_t inlines_end = find_bracket_end(inlines_arr);
								if (inlines_end != string::npos) {
									string inlines = content.substr(inlines_arr, inlines_end - inlines_arr + 1);
									string link_text = ExtractPandocText(inlines);

									// Find the target array (third element) [url, title]
									size_t target_arr = content.find('[', inlines_end + 1);
									if (target_arr != string::npos) {
										size_t url_quote = content.find('"', target_arr);
										if (url_quote != string::npos) {
											auto [url, url_end] = extract_quoted_string(url_quote);
											result += "[" + link_text + "](" + url + ")";
											// Find end of target array
											size_t target_end = find_bracket_end(target_arr);
											pos = (target_end != string::npos) ? target_end + 1 : url_end;
											continue;
										}
									}
								}
							}
						}
					}
				}
			}
		}

		pos = next_pos + 10;
	}

	return result;
}

// Check if content looks like a Pandoc table format
bool DuckBlockFunctions::IsPandocTableFormat(const string &content) {
	// Pandoc tables start with [[ and contain alignment specs like "t":"Align
	return content.size() > 2 &&
	       content[0] == '[' &&
	       content[1] == '[' &&
	       content.find("\"t\":\"Align") != string::npos;
}

// Parse Pandoc table format into headers and rows
void DuckBlockFunctions::ParsePandocTable(const string &content, vector<string> &headers, vector<vector<string>> &rows) {
	// Pandoc table structure: [caption, alignments, widths, tableHead, tableBodies]
	// We need to find the tableHead and tableBodies sections and extract cell text

	// Find all cell blocks - they typically contain "t":"Plain" or "t":"Para" with inline content
	// Strategy: Find row boundaries by looking for patterns like ],[[ that separate rows

	// First, let's find the header row - it comes after the widths array (array of numbers)
	// Look for pattern like ],[[ after the alignments

	int bracket_depth = 0;
	int array_count = 0;
	size_t head_start = string::npos;
	size_t body_start = string::npos;

	// Count arrays at depth 1 (inside the outer wrapper array)
	// Structure: [ caption, alignments, widths, tableHead, tableBodies ]
	//              ^1       ^2          ^3      ^4         ^5
	for (size_t i = 0; i < content.size(); i++) {
		char c = content[i];
		if (c == '[') {
			bracket_depth++;
			// Count arrays at depth 1 (just entered an array that's a direct child of the outer array)
			if (bracket_depth == 2) {
				array_count++;
				if (array_count == 4) {
					head_start = i;
				} else if (array_count == 5) {
					body_start = i;
					break;
				}
			}
		} else if (c == ']') {
			bracket_depth--;
		} else if (c == '"') {
			// Skip strings
			i++;
			while (i < content.size() && content[i] != '"') {
				if (content[i] == '\\') i++;
				i++;
			}
		}
	}

	// Extract header cells from tableHead section
	if (head_start != string::npos) {
		size_t head_end = body_start != string::npos ? body_start : content.size();
		string head_section = content.substr(head_start, head_end - head_start);

		// Find cells in head - look for Plain or Para blocks
		size_t cell_pos = 0;
		while (cell_pos < head_section.size()) {
			size_t plain_pos = head_section.find("\"t\":\"Plain\"", cell_pos);
			size_t para_pos = head_section.find("\"t\":\"Para\"", cell_pos);

			size_t block_pos = string::npos;
			if (plain_pos != string::npos) block_pos = plain_pos;
			if (para_pos != string::npos && (block_pos == string::npos || para_pos < block_pos)) {
				block_pos = para_pos;
			}

			if (block_pos == string::npos) break;

			// Find the "c" array for this block
			size_t c_pos = head_section.find("\"c\":", block_pos);
			if (c_pos != string::npos && c_pos < block_pos + 30) {
				size_t arr_start = head_section.find('[', c_pos);
				if (arr_start != string::npos) {
					// Find matching ]
					int depth = 1;
					size_t arr_end = arr_start + 1;
					while (arr_end < head_section.size() && depth > 0) {
						if (head_section[arr_end] == '[') depth++;
						else if (head_section[arr_end] == ']') depth--;
						else if (head_section[arr_end] == '"') {
							arr_end++;
							while (arr_end < head_section.size() && head_section[arr_end] != '"') {
								if (head_section[arr_end] == '\\') arr_end++;
								arr_end++;
							}
						}
						arr_end++;
					}

					string cell_content = head_section.substr(arr_start, arr_end - arr_start);
					string cell_text = ExtractPandocText(cell_content);
					if (!cell_text.empty()) {
						headers.push_back(cell_text);
					}
					cell_pos = arr_end;
					continue;
				}
			}
			cell_pos = block_pos + 10;
		}
	}

	// Extract body rows from tableBodies section
	if (body_start != string::npos) {
		string body_section = content.substr(body_start);

		// Find row boundaries - look for sequences of cells
		// Each row contains multiple Plain/Para blocks
		vector<string> current_row;
		size_t cell_pos = 0;
		size_t last_cell_end = 0;

		while (cell_pos < body_section.size()) {
			size_t plain_pos = body_section.find("\"t\":\"Plain\"", cell_pos);
			size_t para_pos = body_section.find("\"t\":\"Para\"", cell_pos);

			size_t block_pos = string::npos;
			if (plain_pos != string::npos) block_pos = plain_pos;
			if (para_pos != string::npos && (block_pos == string::npos || para_pos < block_pos)) {
				block_pos = para_pos;
			}

			if (block_pos == string::npos) break;

			// Check if we've crossed a row boundary (large gap or specific pattern)
			// Row boundaries in Pandoc are marked by ]],[[ patterns
			string between = body_section.substr(last_cell_end, block_pos - last_cell_end);
			if (!current_row.empty() && between.find("]],[[") != string::npos) {
				if (current_row.size() > 0) {
					rows.push_back(current_row);
					current_row.clear();
				}
			}

			// Find the "c" array for this block
			size_t c_pos = body_section.find("\"c\":", block_pos);
			if (c_pos != string::npos && c_pos < block_pos + 30) {
				size_t arr_start = body_section.find('[', c_pos);
				if (arr_start != string::npos) {
					// Find matching ]
					int depth = 1;
					size_t arr_end = arr_start + 1;
					while (arr_end < body_section.size() && depth > 0) {
						if (body_section[arr_end] == '[') depth++;
						else if (body_section[arr_end] == ']') depth--;
						else if (body_section[arr_end] == '"') {
							arr_end++;
							while (arr_end < body_section.size() && body_section[arr_end] != '"') {
								if (body_section[arr_end] == '\\') arr_end++;
								arr_end++;
							}
						}
						arr_end++;
					}

					string cell_content = body_section.substr(arr_start, arr_end - arr_start);
					string cell_text = ExtractPandocText(cell_content);
					current_row.push_back(cell_text);
					last_cell_end = arr_end;
					cell_pos = arr_end;
					continue;
				}
			}
			cell_pos = block_pos + 10;
		}

		// Don't forget the last row
		if (!current_row.empty()) {
			rows.push_back(current_row);
		}
	}

	// If we got rows but no headers, and rows have consistent column counts,
	// use the first row as headers
	if (headers.empty() && !rows.empty()) {
		headers = rows[0];
		rows.erase(rows.begin());
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

string DuckBlockFunctions::RenderBlockElementToMarkdown(const string &element_type, const string &content, int32_t level,
                                                        const string &encoding, const Value &attributes) {
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
		if (heading_level < 1) heading_level = 1;
		if (heading_level > 6) heading_level = 6;
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
                                                      const string &content, int32_t level,
                                                      const string &encoding, const Value &attributes) {
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
		    element_type == "list" || element_type == "table" || element_type == "hr" ||
		    element_type == "metadata" || element_type == "frontmatter" || element_type == "code" ||
		    element_type == "image") {
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
	ScalarFunction duck_blocks_to_md(
	    "duck_blocks_to_md", {duck_block_list_type}, markdown_type,
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
					    if (heading_level < 1) heading_level = 1;
					    if (heading_level > 6) heading_level = 6;

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
						    current_section_id = content;
						    std::transform(current_section_id.begin(), current_section_id.end(),
						                   current_section_id.begin(), ::tolower);
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

void DuckBlockFunctions::Register(ExtensionLoader &loader) {
	RegisterDuckBlockToMdFunction(loader);
	RegisterDuckBlocksToMdFunction(loader);
	RegisterDuckBlocksToSectionsFunction(loader);
}

} // namespace duckdb
