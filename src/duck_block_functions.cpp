#include "duck_block_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"
#include "duck_block_vocabulary.hpp"
#include <sstream>

namespace duckdb {

using namespace duckdb_yyjson;

// The duck_block vocabulary, consumed from duck_block_utils as a submodule.
// Spelling these as constants rather than string literals is what makes a
// renamed or removed element type a compile error here instead of output that
// silently stops matching.
using Vocab = DuckBlockVocabulary;

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

// Pandoc list JSON. BulletList is [[block,...], ...]; OrderedList wraps the same
// item array in [[start, style, delim], [...]]. Neither is the ["a", "b"] shape
// ParseJsonListItems reads, so a real producer's list parsed to nothing and the
// items were dropped without a trace. Sets `start` only for the ordered form,
// so an explicit start attribute keeps precedence for the legacy shape.
static bool ParsePandocListItems(const string &content, vector<string> &items, int &start, bool start_from_attribute) {
	if (content.empty()) {
		return false;
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	yyjson_val *item_array = root;
	if (yyjson_is_arr(root) && yyjson_arr_size(root) == 2) {
		yyjson_val *first = yyjson_arr_get(root, 0);
		yyjson_val *second = yyjson_arr_get(root, 1);
		// The ordered form is distinguishable because its leading triple starts
		// with the list's first number; a bullet list's first item is an array of
		// block objects.
		if (yyjson_is_arr(first) && yyjson_arr_size(first) == 3 && yyjson_is_int(yyjson_arr_get(first, 0)) &&
		    yyjson_is_arr(second)) {
			if (!start_from_attribute) {
				start = static_cast<int>(yyjson_get_int(yyjson_arr_get(first, 0)));
			}
			item_array = second;
		}
	}
	if (yyjson_is_arr(item_array)) {
		size_t idx, max;
		yyjson_val *item;
		yyjson_arr_foreach(item_array, idx, max, item) {
			string text = ExtractPandocTextFromVal(item, 0);
			if (!text.empty()) {
				items.push_back(std::move(text));
			}
		}
	}
	yyjson_doc_free(doc);
	return !items.empty();
}

// Flatten a Pandoc block array to text, keeping paragraph breaks between the
// top-level blocks. Returns "" when the content is not a Pandoc block array, so
// callers can fall back rather than replace real content with nothing.
static string ExtractPandocBlocksText(const string &content) {
	if (content.empty()) {
		return "";
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return "";
	}
	string result;
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (yyjson_is_arr(root)) {
		size_t idx, max;
		yyjson_val *block;
		yyjson_arr_foreach(root, idx, max, block) {
			string text = ExtractPandocTextFromVal(block, 0);
			if (text.empty()) {
				continue;
			}
			if (!result.empty()) {
				result += "\n\n";
			}
			result += text;
		}
	}
	yyjson_doc_free(doc);
	return result;
}

// Pandoc DefinitionList JSON: [ [ [term inlines], [ [def blocks], ... ] ], ... ].
// Definitions are lists of blocks; ExtractPandocTextFromVal already flattens Para
// and Plain, so a multi-block definition concatenates rather than being lost.
static bool ParseDefinitionList(const string &content, vector<std::pair<string, vector<string>>> &entries) {
	if (content.empty()) {
		return false;
	}
	yyjson_doc *doc = yyjson_read(content.c_str(), content.size(), 0);
	if (!doc) {
		return false;
	}
	yyjson_val *root = yyjson_doc_get_root(doc);
	if (yyjson_is_arr(root)) {
		size_t idx, max;
		yyjson_val *entry;
		yyjson_arr_foreach(root, idx, max, entry) {
			if (!yyjson_is_arr(entry) || yyjson_arr_size(entry) < 2) {
				continue;
			}
			string term = ExtractPandocTextFromVal(yyjson_arr_get(entry, 0), 0);
			vector<string> definitions;
			yyjson_val *def_list = yyjson_arr_get(entry, 1);
			if (yyjson_is_arr(def_list)) {
				size_t d_idx, d_max;
				yyjson_val *def;
				yyjson_arr_foreach(def_list, d_idx, d_max, def) {
					string text = ExtractPandocTextFromVal(def, 0);
					if (!text.empty()) {
						definitions.push_back(std::move(text));
					}
				}
			}
			entries.emplace_back(std::move(term), std::move(definitions));
		}
	}
	yyjson_doc_free(doc);
	return !entries.empty();
}

// Make a string safe to sit inside an HTML comment: "--" terminates one early, so
// a source_type from an untrusted document must not be able to close it.
static string SanitizeForHtmlComment(const string &text) {
	string safe;
	safe.reserve(text.size());
	for (char c : text) {
		if (c == '\n' || c == '\r') {
			safe += ' ';
		} else if (c == '-' && !safe.empty() && safe.back() == '-') {
			continue;
		} else {
			safe += c;
		}
	}
	return safe;
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
			while (yyjson_arr_size(rows_arr) == 1) {
				yyjson_val *first_row = yyjson_arr_get(rows_arr, 0);
				if (!first_row || !yyjson_is_arr(first_row)) {
					break;
				}
				yyjson_val *first_elem = yyjson_arr_get(first_row, 0);
				if (!first_elem || !yyjson_is_arr(first_elem)) {
					break;
				}
				yyjson_val *first_inner = yyjson_arr_get(first_elem, 0);
				if (!first_inner || !yyjson_is_arr(first_inner)) {
					break;
				}
				rows_arr = first_row;
			}

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
	if (element_type == Vocab::INLINE_LINK) {
		// [text](href "title")
		string href = GetAttribute(attributes, "href");
		string title = GetAttribute(attributes, "title");
		string result = "[" + content + "](" + href;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")";
		return result;
	} else if (element_type == Vocab::INLINE_IMAGE) {
		// ![alt](src "title")
		string src = GetAttribute(attributes, "src");
		string title = GetAttribute(attributes, "title");
		string result = "![" + content + "](" + src;
		if (!title.empty()) {
			result += " \"" + title + "\"";
		}
		result += ")";
		return result;
	} else if (element_type == Vocab::INLINE_BOLD || element_type == "strong") {
		// **text**
		return "**" + content + "**";
	} else if (element_type == Vocab::INLINE_ITALIC || element_type == "emphasis" || element_type == "em") {
		// *text*
		return "*" + content + "*";
	} else if (element_type == Vocab::INLINE_CODE) {
		// `text` - inline code
		// Handle content containing backticks
		if (content.find('`') != string::npos) {
			return "`` " + content + " ``";
		}
		return "`" + content + "`";
	} else if (element_type == Vocab::INLINE_TEXT) {
		// Plain text
		return content;
	} else if (element_type == Vocab::INLINE_SPACE) {
		// Word separator
		return " ";
	} else if (element_type == Vocab::INLINE_SOFTBREAK) {
		// Soft line break
		return "\n";
	} else if (element_type == Vocab::INLINE_LINEBREAK || element_type == "br") {
		// Hard line break
		return "  \n";
	} else if (element_type == Vocab::INLINE_STRIKETHROUGH || element_type == "del") {
		// ~~text~~
		return "~~" + content + "~~";
	} else if (element_type == Vocab::INLINE_SUPERSCRIPT || element_type == "sup") {
		// ^text^
		return "^" + content + "^";
	} else if (element_type == Vocab::INLINE_SUBSCRIPT || element_type == "sub") {
		// ~text~
		return "~" + content + "~";
	} else if (element_type == Vocab::INLINE_UNDERLINE) {
		// <u>text</u> (no standard markdown, use HTML)
		return "<u>" + content + "</u>";
	} else if (element_type == Vocab::INLINE_SMALLCAPS) {
		// No standard markdown for small caps, use span with style
		return "<span style=\"font-variant: small-caps\">" + content + "</span>";
	} else if (element_type == Vocab::INLINE_MATH) {
		// $text$ for inline, $$text$$ for display
		string display = GetAttribute(attributes, "display");
		if (display == "block") {
			return "$$" + content + "$$";
		}
		return "$" + content + "$";
	} else if (element_type == Vocab::INLINE_RAW) {
		// Raw content as-is
		return content;
	} else if (element_type == Vocab::INLINE_QUOTED) {
		// Quoted text
		string quote_type = GetAttribute(attributes, "quote_type");
		if (quote_type == "single") {
			return "'" + content + "'";
		}
		return "\"" + content + "\"";
	} else if (element_type == Vocab::INLINE_CITE) {
		// Citation [@key]
		string key = GetAttribute(attributes, "key");
		if (!key.empty()) {
			return "[@" + key + "]";
		}
		return content;
	} else if (element_type == Vocab::INLINE_NOTE) {
		// Footnote [^note]
		return "[^" + content + "]";
	} else if (element_type == Vocab::INLINE_SPAN) {
		// Generic span - just output content
		return content;
	} else if (element_type == Vocab::INLINE_GENERIC) {
		// The inline backstop for a construct outside this vocabulary. Unlike its
		// block counterpart it carries no content of its own -- the text is in its
		// children, which reach us as `content` -- so passing it through loses only
		// the wrapper identity, not the words. A comment would interrupt the prose
		// it sits inside, so this is deliberately quieter than the block case.
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

	if (element_type == "frontmatter" || element_type == Vocab::TYPE_METADATA) {
		// YAML frontmatter
		result = "---\n" + content + "\n---\n\n";
	} else if (element_type == Vocab::TYPE_HEADING) {
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
	} else if (element_type == Vocab::TYPE_PARAGRAPH) {
		// Plain paragraph
		result = content + "\n\n";
	} else if (element_type == Vocab::TYPE_CODE) {
		// Fenced code block
		string language = GetAttribute(attributes, "language");
		result = "```" + language + "\n" + content + "\n```\n\n";
	} else if (element_type == Vocab::TYPE_BLOCKQUOTE) {
		// Block quote - add > prefix to each line.
		// Producers emit the quote's blocks as a Pandoc array rather than as text
		// (the released duck_block_utils and its main both do), and prefixing that
		// verbatim put raw AST into the document behind a '>'.
		string source = content;
		if (encoding == Vocab::ENCODING_JSON) {
			string extracted = ExtractPandocBlocksText(content);
			if (!extracted.empty()) {
				source = extracted;
			}
		}
		string quoted;
		std::istringstream iss(source);
		std::string line;
		while (std::getline(iss, line)) {
			quoted += "> " + line + "\n";
		}
		result = quoted + "\n";
	} else if (element_type == Vocab::TYPE_LIST) {
		// List - content is JSON encoded
		if (encoding == "json" && content.length() > 2 && content[0] == '[') {
			// `list_type` is CANONICAL for orderedness; `ordered` is a legacy
			// alias. A boolean cannot grow, and the type set already wants values
			// -- definition and navigation lists -- that `ordered=false` can only
			// say a list is not. Both producers emit both names, so this decides
			// only which one wins when they disagree.
			const string list_type = GetAttribute(attributes, "list_type");
			const bool ordered =
			    !list_type.empty() ? (list_type == "ordered") : (GetAttribute(attributes, "ordered") == "true");
			int start = 1;
			string start_str = GetAttribute(attributes, "start");
			const bool start_from_attribute = !start_str.empty();
			if (start_from_attribute) {
				try {
					start = std::stoi(start_str);
				} catch (...) {
				}
			}

			auto items = ParseJsonListItems(content);
			if (items.empty()) {
				ParsePandocListItems(content, items, start, start_from_attribute);
			}
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
	} else if (element_type == Vocab::TYPE_TABLE) {
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
	} else if (element_type == Vocab::TYPE_HR) {
		result = "---\n\n";
	} else if (element_type == Vocab::TYPE_LIST_ITEM) {
		// List item - render with bullet prefix
		// Check if ordered from attributes
		bool ordered = GetAttribute(attributes, "ordered") == "true";
		string item_num = GetAttribute(attributes, "item_number");
		if (ordered && !item_num.empty()) {
			result = item_num + ". " + content + "\n";
		} else {
			result = "- " + content + "\n";
		}
	} else if (element_type == Vocab::TYPE_IMAGE) {
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
	} else if (element_type == Vocab::TYPE_RAW || element_type == "html" || element_type == "md:html_block") {
		// Raw content - output as-is
		result = content + "\n\n";
	} else if (element_type == Vocab::TYPE_DIV || element_type == Vocab::TYPE_SECTION ||
	           element_type == Vocab::TYPE_FIGURE || element_type == Vocab::TYPE_CAPTION) {
		// A container's children are usually its content, and they render
		// themselves -- so the marker emits nothing and no stray blank line.
		//
		// But a container whose only child is a text run carries that text in its
		// OWN content (spec 6.0), so dropping content unconditionally lost it. The
		// same rule had list_item dropping the text of every tight item; fixing it
		// only there left this in four more places, which is why the sentinel
		// sweep found them and a round trip never could.
		if (content.empty()) {
			result = "";
		} else if (element_type == Vocab::TYPE_CAPTION) {
			// Marked as a caption, as the scoped form is.
			result = "*" + content + "*\n\n";
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == Vocab::TYPE_LINEBLOCK) {
		// Preserved line breaks. Markdown has no line-block syntax, and a bare
		// newline inside a paragraph is a soft break that collapses to a space --
		// so terminate each line with a hard break instead.
		std::istringstream iss(content);
		string line;
		vector<string> lines;
		while (std::getline(iss, line)) {
			// Trailing whitespace is stripped before the break is re-added: a rich
			// line block arrives as a rendered inline run whose `linebreak`
			// children have ALREADY emitted "  \n", and joining that again gave
			// four trailing spaces. A plain-text one has none to strip, so it is
			// unaffected.
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
				line.pop_back();
			}
			lines.push_back(line);
		}
		for (size_t i = 0; i < lines.size(); i++) {
			result += lines[i];
			if (i + 1 < lines.size()) {
				result += "  \n";
			}
		}
		result += "\n\n";
	} else if (element_type == Vocab::TYPE_DEFLIST) {
		// Definition list (Markdown Extra / Pandoc syntax)
		vector<std::pair<string, vector<string>>> entries;
		if (ParseDefinitionList(content, entries)) {
			for (const auto &entry : entries) {
				result += entry.first + "\n";
				for (const auto &definition : entry.second) {
					result += ": " + definition + "\n";
				}
				result += "\n";
			}
		} else {
			result = content + "\n\n";
		}
	} else if (element_type == Vocab::TYPE_PAGE) {
		// A physical pagination boundary, carrying no content. Markdown has no
		// page-break syntax, and a `---` here would read as a thematic break (or,
		// at the top of a document, as frontmatter delimiters), so leave the same
		// kind of greppable trace `generic` does rather than lose the boundary.
		string page_number = SanitizeForHtmlComment(GetAttribute(attributes, "page_number"));
		if (page_number.empty()) {
			result = "<!-- page break -->\n\n";
		} else {
			result = "<!-- page break: " + page_number + " -->\n\n";
		}
	} else if (element_type == Vocab::TYPE_GENERIC) {
		// "Structurally valid, type not in this vocabulary." The content is a
		// verbatim source fragment that would be noise in a markdown document, but
		// dropping it silently is what `generic` exists to prevent -- so name the
		// construct and leave a trace a reader can grep for.
		string source_type = SanitizeForHtmlComment(GetAttribute(attributes, "source_type"));
		if (source_type.empty()) {
			result = "<!-- unsupported block -->\n\n";
		} else {
			result = "<!-- unsupported block: " + source_type + " -->\n\n";
		}
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
	if (kind == Vocab::KIND_BLOCK) {
		// Delegate to block rendering
		return RenderBlockElementToMarkdown(element_type, content, level, encoding, attributes);
	} else if (kind == Vocab::KIND_INLINE) {
		// Use inline rendering
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	} else {
		// Unknown kind - try to guess based on element_type
		// Block types
		if (element_type == Vocab::TYPE_HEADING || element_type == Vocab::TYPE_PARAGRAPH ||
		    element_type == Vocab::TYPE_BLOCKQUOTE || element_type == Vocab::TYPE_LIST ||
		    element_type == Vocab::TYPE_TABLE || element_type == Vocab::TYPE_HR ||
		    element_type == Vocab::TYPE_METADATA || element_type == "frontmatter" || element_type == Vocab::TYPE_CODE ||
		    element_type == Vocab::TYPE_IMAGE) {
			return RenderBlockElementToMarkdown(element_type, content, level, encoding, attributes);
		}
		// Assume inline otherwise
		return RenderInlineElementToMarkdown(element_type, content, attributes);
	}
}

//===--------------------------------------------------------------------===//
// Kind filtering (duck_block `kind` is an open discriminator)
//===--------------------------------------------------------------------===//

// The seven core duck_block fields. Two optional trailing fields (source_format,
// file_path) may follow, so this is a minimum, not an exact width.
static constexpr idx_t DUCK_BLOCK_FIELD_COUNT = Vocab::ELEMENT_ORDER_IDX + 1;

static string DuckBlockKind(const Value &block_value) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::KIND_IDX].IsNull()) {
		return "";
	}
	return fields[Vocab::KIND_IDX].ToString();
}

// `level` is structural nesting DEPTH, so a top-level element has depth 1 and
// records NULL because it is not nested. Reading NULL as 0 also makes a scope
// walk work, but it puts a top-level element one level shallower than its
// siblings that do carry a level, so an element at level 1 next to one at NULL
// reads as its child rather than its sibling.
static constexpr int32_t DUCK_BLOCK_TOP_LEVEL = 1;

static int32_t DuckBlockLevel(const Value &block_value) {
	if (block_value.IsNull()) {
		return DUCK_BLOCK_TOP_LEVEL;
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::LEVEL_IDX].IsNull()) {
		return DUCK_BLOCK_TOP_LEVEL;
	}
	return fields[Vocab::LEVEL_IDX].GetValue<int32_t>();
}

// `kind` gained a third value ('value', carrying document metadata) after this
// writer was written, and more may follow. Allowlist what we know how to render:
// a blocklist ("not inline, so it must be a block") turns every future kind into
// body prose, which is how document titles ended up appended to documents.
// An empty kind is legacy data and keeps its element_type-based guess.
static bool IsRenderableKind(const string &kind) {
	return kind.empty() || kind == Vocab::KIND_BLOCK || kind == Vocab::KIND_INLINE;
}

// A non-renderable element's children must go with it. Metadata nests by level
// exactly as block containers do -- the scope runs from the marker to the first
// element back at its own level -- and those children are ordinary kinds:
// MetaInlines carries kind='inline' children, MetaBlocks carries kind='block'
// ones. Skipping only the marker leaves them to render as stray prose.
// Returns the index just past the scope opened at `marker_index`, never past `end`.
static idx_t SkipElementScope(const vector<Value> &children, idx_t marker_index, idx_t end) {
	const int32_t scope_level = DuckBlockLevel(children[marker_index]);
	idx_t i = marker_index + 1;
	while (i < end && DuckBlockLevel(children[i]) > scope_level) {
		i++;
	}
	return i;
}

// Caption scopes are rendered by recursing into the range they open. Each level
// consumes at least one element, so depth is bounded by the list length -- which
// an adversarial document controls. Past this depth a caption is treated as a
// plain container: its children still render, just without the emphasis.
static constexpr int MAX_DUCK_BLOCK_NESTING = 64;

//===--------------------------------------------------------------------===//
// RenderDuckBlocksToMarkdown (list of duck_blocks)
//===--------------------------------------------------------------------===//

// Read an element's own content, for the walk. Absent is spelled two ways --
// the builders write NULL, the Pandoc reader writes an empty string.
static string DuckBlockContent(const Value &block_value) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT || fields[Vocab::CONTENT_IDX].IsNull()) {
		return "";
	}
	return fields[Vocab::CONTENT_IDX].ToString();
}

// Read an attribute straight off a duck_block Value, for the walk, which has the
// element rather than its already-extracted attributes map.
static string GetAttributeOf(const Value &block_value, const string &key) {
	if (block_value.IsNull()) {
		return "";
	}
	auto &fields = StructValue::GetChildren(block_value);
	if (fields.size() < DUCK_BLOCK_FIELD_COUNT) {
		return "";
	}
	return DuckBlockFunctions::GetAttribute(fields[Vocab::ATTRIBUTES_IDX], key);
}

// Ordered-list markers. Pandoc carries the style and delimiter separately from
// the number, so a list starting at iii) round-trips as iii) rather than 3.
static string RomanNumeral(int n, bool upper) {
	static const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
	static const char *lower[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
	static const char *upper_s[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
	string out;
	for (size_t i = 0; i < 13 && n > 0; i++) {
		while (n >= values[i]) {
			out += upper ? upper_s[i] : lower[i];
			n -= values[i];
		}
	}
	return out;
}

static string AlphaNumeral(int n, bool upper) {
	string out;
	while (n > 0) {
		const int rem = (n - 1) % 26;
		out.insert(out.begin(), static_cast<char>((upper ? 'A' : 'a') + rem));
		n = (n - 1) / 26;
	}
	return out;
}

static string FormatListMarker(int number, const string &style, const string &delim) {
	string text;
	if (number < 1) {
		number = 1;
	}
	if (style == "LowerRoman") {
		text = RomanNumeral(number, false);
	} else if (style == "UpperRoman") {
		text = RomanNumeral(number, true);
	} else if (style == "LowerAlpha") {
		text = AlphaNumeral(number, false);
	} else if (style == "UpperAlpha") {
		text = AlphaNumeral(number, true);
	} else {
		// Decimal, Example, DefaultStyle, and anything this build does not know.
		text = std::to_string(number);
	}
	return text + ((delim == "OneParen" || delim == "TwoParens") ? ")" : ".") + " ";
}

// Prefix the first line with `marker` and indent the rest to line up under it.
// Blank lines stay genuinely blank rather than becoming trailing whitespace.
static string IndentContinuation(const string &text, const string &marker) {
	const string pad(marker.size(), ' ');
	string out;
	std::istringstream iss(text);
	string line;
	bool first = true;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (first) {
			out += marker + line + "\n";
			first = false;
		} else if (line.empty()) {
			out += "\n";
		} else {
			out += pad + line + "\n";
		}
	}
	if (first) {
		// Nothing to indent; still emit the marker so the item is not lost.
		out += marker + "\n";
	}
	return out;
}

// Render [begin, end) of a duck_block list. Recursive, because a `caption` has to
// be rendered as a unit before it can be decorated -- markdown has no caption
// syntax, so the alternative is prose no reader can tell from body text.
static string RenderDuckBlockRange(const vector<Value> &list_children, idx_t begin, idx_t end, int depth) {
	string result;
	bool last_was_inline = false;

	for (idx_t i = begin; i < end;) {
		const auto &block_value = list_children[i];
		if (block_value.IsNull()) {
			i++;
			continue;
		}

		auto &struct_children = StructValue::GetChildren(block_value);
		if (struct_children.size() < DUCK_BLOCK_FIELD_COUNT) {
			i++;
			continue;
		}

		// duck_block: kind, element_type, content, level, encoding, attributes, element_order
		string kind = struct_children[Vocab::KIND_IDX].IsNull() ? "" : struct_children[Vocab::KIND_IDX].ToString();

		// Metadata and any future kind are not document content; drop the whole scope.
		if (!IsRenderableKind(kind)) {
			i = SkipElementScope(list_children, i, end);
			continue;
		}
		string element_type = struct_children[Vocab::ELEMENT_TYPE_IDX].IsNull()
		                          ? ""
		                          : struct_children[Vocab::ELEMENT_TYPE_IDX].ToString();
		string content =
		    struct_children[Vocab::CONTENT_IDX].IsNull() ? "" : struct_children[Vocab::CONTENT_IDX].ToString();
		int32_t level =
		    struct_children[Vocab::LEVEL_IDX].IsNull() ? 0 : struct_children[Vocab::LEVEL_IDX].GetValue<int32_t>();
		string encoding =
		    struct_children[Vocab::ENCODING_IDX].IsNull() ? "text" : struct_children[Vocab::ENCODING_IDX].ToString();
		Value attributes = struct_children[Vocab::ATTRIBUTES_IDX];

		// Spec 1.2 made `list` and `blockquote` structural: the container carries
		// no content of its own and its blocks follow as children by level. The
		// older shape packed the whole thing into JSON content and is still what
		// released producers emit, so the block renderer keeps handling that and
		// these paths only take over when there is no content to render.
		if (kind == Vocab::KIND_BLOCK && content.empty() && depth < MAX_DUCK_BLOCK_NESTING) {
			if (element_type == Vocab::TYPE_LIST) {
				const idx_t scope_end = SkipElementScope(list_children, i, end);
				if (scope_end > i + 1) {
					// Canonical `list_type` first, then the legacy `ordered` alias.
					const string list_type = GetAttributeOf(block_value, "list_type");
					const bool ordered = !list_type.empty() ? (list_type == "ordered")
					                                        : (GetAttributeOf(block_value, "ordered") == "true");
					const string number_style = GetAttributeOf(block_value, "number_style");
					const string number_delim = GetAttributeOf(block_value, "number_delim");
					int number = 1;
					const string start_attr = GetAttributeOf(block_value, "start");
					if (!start_attr.empty()) {
						try {
							number = std::stoi(start_attr);
						} catch (...) {
						}
					}
					const int32_t item_level = DuckBlockLevel(list_children[i]) + 1;
					if (last_was_inline) {
						result += "\n\n";
					}

					// A definition list is a `list` whose items are terms and
					// definitions as SIBLINGS at one level, paired by order: a term
					// owns the role='definition' items following it until the next
					// term. One term may own several definitions, which is distinct
					// from one definition of several blocks -- many items versus one
					// item with children. Measured off the live producer rather than
					// taken from its description.
					if (list_type == "definition") {
						bool first_term = true;
						for (idx_t j = i + 1; j < scope_end;) {
							if (DuckBlockLevel(list_children[j]) != item_level) {
								const idx_t stray_end = SkipElementScope(list_children, j, scope_end);
								result += RenderDuckBlockRange(list_children, j, stray_end, depth + 1);
								j = stray_end;
								continue;
							}
							const idx_t item_end = SkipElementScope(list_children, j, scope_end);
							string text = item_end > j + 1
							                  ? RenderDuckBlockRange(list_children, j + 1, item_end, depth + 1)
							                  : DuckBlockContent(list_children[j]);
							StringUtil::Trim(text);
							// Anything not marked a definition is a label: an item whose
							// role is missing reads as a term rather than disappearing.
							if (GetAttributeOf(list_children[j], "role") == "definition") {
								// ":" plus three spaces, so a continuation block lands at
								// four. Two was not enough: a reader takes a 2-space block
								// as a new top-level paragraph, which splits the list in
								// half and strands the block between the pieces. This is
								// also Pandoc's own output width, so marker and pad agree.
								result += IndentContinuation(text, ":   ");
							} else {
								if (!first_term) {
									result += "\n";
								}
								result += text + "\n";
								first_term = false;
							}
							j = item_end;
						}
						result += "\n";
						last_was_inline = false;
						i = scope_end;
						continue;
					}

					for (idx_t j = i + 1; j < scope_end;) {
						if (DuckBlockLevel(list_children[j]) != item_level) {
							// Not an item -- a producer we do not recognise, or a
							// shape from a spec we have not seen. Render it anyway
							// rather than advancing past it: this decides whether
							// the child EXISTS, and that must not be keyed on it
							// matching a shape we know. Losing the list structure
							// costs formatting; dropping it costs the text.
							const idx_t stray_end = SkipElementScope(list_children, j, scope_end);
							result += RenderDuckBlockRange(list_children, j, stray_end, depth + 1);
							j = stray_end;
							continue;
						}
						const idx_t item_end = SkipElementScope(list_children, j, scope_end);
						// An item's children are its content when it has any; its own
						// content is its content when it does not. Rendering only the
						// child scope dropped the text of every item that carried it
						// directly, which is the shape the live producer emits.
						string inner = item_end > j + 1
						                   ? RenderDuckBlockRange(list_children, j + 1, item_end, depth + 1)
						                   : DuckBlockContent(list_children[j]);
						StringUtil::Trim(inner);
						const string marker = ordered ? FormatListMarker(number++, number_style, number_delim) : "- ";
						result += IndentContinuation(inner, marker);
						j = item_end;
					}
					result += "\n";
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			} else if (element_type == Vocab::TYPE_LIST_ITEM) {
				// An item met without its container -- render it as a bullet rather
				// than dropping the blocks it holds.
				const idx_t scope_end = SkipElementScope(list_children, i, end);
				if (scope_end > i + 1) {
					string inner = RenderDuckBlockRange(list_children, i + 1, scope_end, depth + 1);
					StringUtil::Trim(inner);
					if (last_was_inline) {
						result += "\n\n";
					}
					result += IndentContinuation(inner, "- ") + "\n";
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			} else if (element_type == Vocab::TYPE_BLOCKQUOTE) {
				const idx_t scope_end = SkipElementScope(list_children, i, end);
				if (scope_end > i + 1) {
					string inner = RenderDuckBlockRange(list_children, i + 1, scope_end, depth + 1);
					StringUtil::Trim(inner);
					if (last_was_inline) {
						result += "\n\n";
					}
					std::istringstream iss(inner);
					string line;
					while (std::getline(iss, line)) {
						if (!line.empty() && line.back() == '\r') {
							line.pop_back();
						}
						result += line.empty() ? ">\n" : "> " + line + "\n";
					}
					result += "\n";
					last_was_inline = false;
					i = scope_end;
					continue;
				}
			}
		}

		// A caption's children are its content, so render the scope it opens and
		// mark the result. Position is the producer's choice -- a figure caption
		// follows its content, a disclosure summary precedes its body -- so this
		// renders in place and imposes no order of its own.
		if (kind == Vocab::KIND_BLOCK && element_type == Vocab::TYPE_CAPTION && depth < MAX_DUCK_BLOCK_NESTING) {
			idx_t scope_end = SkipElementScope(list_children, i, end);
			// Children are the caption's text when it has any; its own content is
			// its text when it does not. Without the fallback this branch consumed
			// a childless caption and emitted nothing, which also shadowed the leaf
			// renderer -- so the text was lost twice over.
			string inner = scope_end > i + 1 ? RenderDuckBlockRange(list_children, i + 1, scope_end, depth + 1)
			                                 : DuckBlockContent(list_children[i]);
			StringUtil::Trim(inner);
			if (!inner.empty()) {
				if (last_was_inline) {
					result += "\n\n";
				}
				// Emphasis cannot span a paragraph break, so only a single-block
				// caption gets marked; a multi-block one is emitted as it stands.
				if (inner.find("\n\n") == string::npos) {
					result += "*" + inner + "*";
				} else {
					result += inner;
				}
				result += "\n\n";
			}
			last_was_inline = false;
			i = scope_end;
			continue;
		}

		// An element with no content of its own carries it as kind='inline'
		// children -- a paragraph whose text is a run of inlines, a bold whose
		// text nests deeper. Walking those flat rendered the marker's empty
		// content as an element in its own right, which put a blank paragraph
		// ahead of the text and closed "**" around nothing.
		//
		// Blocks and inlines sit on SEPARATE level scales: the spec puts a
		// top-level inline at level 1 while a top-level block records NULL. And
		// producers disagree about where a block's inline run begins -- the
		// builders and all four panduck readers emit level 1, the Pandoc path
		// emits 2. So a block takes the whole contiguous inline run that follows
		// it rather than trusting the level, which is the only rule that reads
		// both; among inlines the level IS the nesting, so a wrapper there takes
		// what is deeper than itself.
		idx_t scope_end;
		if (kind == Vocab::KIND_BLOCK) {
			scope_end = i + 1;
			while (scope_end < end && DuckBlockKind(list_children[scope_end]) == Vocab::KIND_INLINE) {
				scope_end++;
			}
		} else {
			scope_end = SkipElementScope(list_children, i, end);
		}
		bool consumed_children = false;
		if (content.empty() && scope_end > i + 1 && depth < MAX_DUCK_BLOCK_NESTING &&
		    DuckBlockKind(list_children[i + 1]) == Vocab::KIND_INLINE) {
			content = RenderDuckBlockRange(list_children, i + 1, scope_end, depth + 1);
			consumed_children = true;
		}

		bool is_inline = (kind == Vocab::KIND_INLINE);

		// Handle inline-to-block transition: add paragraph break
		if (last_was_inline && !is_inline) {
			result += "\n\n";
		}

		result +=
		    DuckBlockFunctions::RenderDuckBlockToMarkdown(kind, element_type, content, level, encoding, attributes);
		last_was_inline = is_inline;
		i = consumed_children ? scope_end : i + 1;
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Document metadata -> YAML frontmatter
//===--------------------------------------------------------------------===//

// Every scalar is double-quoted. It costs a little noise and buys the guarantee
// that a value out of an untrusted document -- one containing a newline and a
// `---`, say -- cannot forge frontmatter structure around itself.
static string YamlQuote(const string &text) {
	string out = "\"";
	for (unsigned char c : text) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += ' ';
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += "\"";
	return out;
}

static string YamlKey(const string &key) {
	if (key.empty()) {
		return YamlQuote(key);
	}
	for (size_t i = 0; i < key.size(); i++) {
		const unsigned char c = key[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
		                (i > 0 && ((c >= '0' && c <= '9') || c == '-' || c == '.'));
		if (!ok) {
			return YamlQuote(key);
		}
	}
	return key;
}

// A YAML literal block scalar, for metadata whose value is block content.
static string YamlBlockScalar(const string &text, int indent) {
	string out = "|\n";
	const string pad(indent + 2, ' ');
	std::istringstream iss(text);
	string line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		out += pad + line + "\n";
	}
	return out;
}

// Emit the YAML for the kind='value' scope opening at `i`, which nests by level
// exactly as block containers do. Returns the index just past that scope.
static idx_t RenderMetaValue(const vector<Value> &children, idx_t i, idx_t end, int indent, bool in_sequence,
                             string &out) {
	const idx_t scope_end = SkipElementScope(children, i, end);
	auto &fields = StructValue::GetChildren(children[i]);
	const string element_type =
	    fields[Vocab::ELEMENT_TYPE_IDX].IsNull() ? "" : fields[Vocab::ELEMENT_TYPE_IDX].ToString();
	const string content = fields[Vocab::CONTENT_IDX].IsNull() ? "" : fields[Vocab::CONTENT_IDX].ToString();
	const string key = DuckBlockFunctions::GetAttribute(fields[Vocab::ATTRIBUTES_IDX], "key");

	// The version marker deliberately carries no key, which is what keeps a
	// storage/exchange stamp out of the document's own metadata on export.
	if (element_type == Vocab::VALUE_VERSION) {
		return scope_end;
	}

	const string prefix = string(indent, ' ') + (in_sequence ? "- " : "");
	const string label = key.empty() ? "" : YamlKey(key) + ":";

	if (element_type == Vocab::VALUE_LIST || element_type == Vocab::VALUE_MAP) {
		out += prefix + (label.empty() ? string("-") : label) + "\n";
		const bool children_in_sequence = (element_type == Vocab::VALUE_LIST);
		idx_t j = i + 1;
		while (j < scope_end) {
			if (DuckBlockKind(children[j]) != Vocab::KIND_VALUE) {
				j++;
				continue;
			}
			j = RenderMetaValue(children, j, scope_end, indent + 2, children_in_sequence, out);
		}
		return scope_end;
	}

	string scalar;
	if (element_type == Vocab::VALUE_BOOL) {
		// The one value type emitted bare rather than quoted.
		scalar = (content == "true") ? "true" : "false";
	} else if (element_type == Vocab::VALUE_INLINES) {
		string rendered = RenderDuckBlockRange(children, i + 1, scope_end, 0);
		StringUtil::Trim(rendered);
		scalar = YamlQuote(rendered);
	} else if (element_type == Vocab::VALUE_BLOCKS) {
		string rendered = RenderDuckBlockRange(children, i + 1, scope_end, 0);
		StringUtil::Trim(rendered);
		out += prefix + (label.empty() ? string("-") : label) + " " + YamlBlockScalar(rendered, indent);
		return scope_end;
	} else {
		// string, generic, and any value type this build does not know: the content
		// is the value verbatim.
		scalar = YamlQuote(content);
	}

	if (label.empty()) {
		out += prefix + scalar + "\n";
	} else {
		out += prefix + label + " " + scalar + "\n";
	}
	return scope_end;
}

// Build the frontmatter for a document's kind='value' elements. Empty when the
// document has no metadata.
static string RenderDocumentFrontmatter(const vector<Value> &children) {
	string body;
	for (idx_t i = 0; i < children.size();) {
		if (DuckBlockKind(children[i]) != Vocab::KIND_VALUE) {
			i++;
			continue;
		}
		i = RenderMetaValue(children, i, children.size(), 0, false, body);
	}
	if (body.empty()) {
		return "";
	}
	return "---\n" + body + "---\n\n";
}

// True when the list already carries frontmatter of its own, in which case
// synthesising a second block would produce a document with two.
static bool HasFrontmatterBlock(const vector<Value> &children) {
	for (const auto &child : children) {
		if (DuckBlockKind(child) != Vocab::KIND_BLOCK) {
			continue;
		}
		auto &fields = StructValue::GetChildren(child);
		const string element_type =
		    fields[Vocab::ELEMENT_TYPE_IDX].IsNull() ? "" : fields[Vocab::ELEMENT_TYPE_IDX].ToString();
		if (element_type == Vocab::TYPE_METADATA || element_type == "frontmatter") {
			return true;
		}
	}
	return false;
}

string DuckBlockFunctions::RenderDuckBlocksToMarkdown(const Value &blocks_value) {
	if (blocks_value.IsNull() || blocks_value.type().id() != LogicalTypeId::LIST) {
		return "";
	}
	auto &list_children = ListValue::GetChildren(blocks_value);
	string result;
	// Metadata is not body content, but markdown -- alone among the output formats
	// this vocabulary targets -- has somewhere to put it, so it round-trips as
	// frontmatter instead of being dropped.
	if (!HasFrontmatterBlock(list_children)) {
		result = RenderDocumentFrontmatter(list_children);
	}
	result += RenderDuckBlockRange(list_children, 0, list_children.size(), 0);
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
			    string kind =
			        struct_children[Vocab::KIND_IDX].IsNull() ? "" : struct_children[Vocab::KIND_IDX].ToString();
			    string element_type = struct_children[Vocab::ELEMENT_TYPE_IDX].IsNull()
			                              ? ""
			                              : struct_children[Vocab::ELEMENT_TYPE_IDX].ToString();
			    string content =
			        struct_children[Vocab::CONTENT_IDX].IsNull() ? "" : struct_children[Vocab::CONTENT_IDX].ToString();
			    int32_t level = struct_children[Vocab::LEVEL_IDX].IsNull()
			                        ? 0
			                        : struct_children[Vocab::LEVEL_IDX].GetValue<int32_t>();
			    string encoding = struct_children[Vocab::ENCODING_IDX].IsNull()
			                          ? "text"
			                          : struct_children[Vocab::ENCODING_IDX].ToString();
			    Value attributes = struct_children[Vocab::ATTRIBUTES_IDX];

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
			    vector<string> section_path_parts;

			    // Render a section's content through the SHARED range renderer rather
			    // than element by element. The two walks over a block list have to
			    // agree about how structure renders, and every structural rule --
			    // list items, blockquote nesting, caption emphasis, a block's inline
			    // children -- lives in that renderer. Walking elements individually
			    // here produced a bullet with no text followed by loose prose, while
			    // duck_blocks_to_md produced "- LISTWORD" from the same input.
			    idx_t content_begin = 0;
			    auto render_range = [&](idx_t begin, idx_t end) -> string {
				    return end > begin ? RenderDuckBlockRange(list_children, begin, end, 0) : string();
			    };

			    auto flush_section = [&](const string &body) {
				    if (!current_title.empty() || !body.empty()) {
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
					    section_values.push_back(std::make_pair("content", Value(body)));

					    sections.push_back(Value::STRUCT(section_values));
				    }
			    };

			    for (idx_t i = 0; i < list_children.size();) {
				    const auto &block_value = list_children[i];
				    if (block_value.IsNull()) {
					    i++;
					    continue;
				    }

				    auto &struct_children = StructValue::GetChildren(block_value);
				    if (struct_children.size() < DUCK_BLOCK_FIELD_COUNT) {
					    i++;
					    continue;
				    }

				    // duck_block: kind, element_type, content, level, encoding, attributes, element_order
				    string kind =
				        struct_children[Vocab::KIND_IDX].IsNull() ? "" : struct_children[Vocab::KIND_IDX].ToString();

				    // Metadata and any future kind are not section content.
				    if (!IsRenderableKind(kind)) {
					    i = SkipElementScope(list_children, i, list_children.size());
					    continue;
				    }
				    string element_type = struct_children[Vocab::ELEMENT_TYPE_IDX].IsNull()
				                              ? ""
				                              : struct_children[Vocab::ELEMENT_TYPE_IDX].ToString();
				    string content = struct_children[Vocab::CONTENT_IDX].IsNull()
				                         ? ""
				                         : struct_children[Vocab::CONTENT_IDX].ToString();
				    int32_t level = struct_children[Vocab::LEVEL_IDX].IsNull()
				                        ? 0
				                        : struct_children[Vocab::LEVEL_IDX].GetValue<int32_t>();
				    string encoding = struct_children[Vocab::ENCODING_IDX].IsNull()
				                          ? "text"
				                          : struct_children[Vocab::ENCODING_IDX].ToString();
				    Value attributes = struct_children[Vocab::ATTRIBUTES_IDX];

				    if (element_type == Vocab::TYPE_HEADING) {
					    // Flush previous section
					    flush_section(render_range(content_begin, i));

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
					    content_begin = i + 1;
				    } else if (element_type == Vocab::TYPE_METADATA || element_type == "frontmatter") {
					    // Metadata becomes level 0 section
					    flush_section(render_range(content_begin, i));
					    current_title = "";
					    current_level = 0;
					    current_section_id = "frontmatter";
					    flush_section(content);
					    current_title.clear();
					    content_begin = i + 1;
				    }
				    // Anything else is section content, rendered as a range when the
				    // section is flushed rather than one element at a time.
				    i++;
			    }

			    // Flush final section
			    flush_section(render_range(content_begin, list_children.size()));

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
