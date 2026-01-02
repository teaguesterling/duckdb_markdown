#include "markdown_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <map>

// Include actual Markdown parser headers
#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm-core-extensions.h>

namespace duckdb {

namespace markdown_utils {

//===--------------------------------------------------------------------===//
// Core Conversion Functions
//===--------------------------------------------------------------------===//

std::string MarkdownToHTML(const std::string &markdown_str, MarkdownFlavor flavor) {
	if (markdown_str.empty()) {
		return "";
	}

	// Initialize cmark-gfm
	cmark_gfm_core_extensions_ensure_registered();

	// Parse options based on flavor
	int options = CMARK_OPT_DEFAULT;
	if (flavor == MarkdownFlavor::GFM) {
		options |= CMARK_OPT_GITHUB_PRE_LANG;
	}

	// Parse the markdown document
	cmark_parser *parser = cmark_parser_new(options);

	if (flavor == MarkdownFlavor::GFM) {
		// Add GitHub extensions
		cmark_syntax_extension *table_extension = cmark_find_syntax_extension("table");
		cmark_syntax_extension *strikethrough_extension = cmark_find_syntax_extension("strikethrough");
		cmark_syntax_extension *autolink_extension = cmark_find_syntax_extension("autolink");
		cmark_syntax_extension *tagfilter_extension = cmark_find_syntax_extension("tagfilter");
		cmark_syntax_extension *tasklist_extension = cmark_find_syntax_extension("tasklist");

		if (table_extension)
			cmark_parser_attach_syntax_extension(parser, table_extension);
		if (strikethrough_extension)
			cmark_parser_attach_syntax_extension(parser, strikethrough_extension);
		if (autolink_extension)
			cmark_parser_attach_syntax_extension(parser, autolink_extension);
		if (tagfilter_extension)
			cmark_parser_attach_syntax_extension(parser, tagfilter_extension);
		if (tasklist_extension)
			cmark_parser_attach_syntax_extension(parser, tasklist_extension);
	}

	// Feed the input to the parser
	cmark_parser_feed(parser, markdown_str.c_str(), markdown_str.length());

	// Parse and render
	cmark_node *doc = cmark_parser_finish(parser);
	char *html = cmark_render_html(doc, options, nullptr);

	std::string result(html);

	// Clean up
	free(html);
	cmark_node_free(doc);
	cmark_parser_free(parser);

	return result;
}

std::string MarkdownToText(const std::string &markdown_str) {
	if (markdown_str.empty()) {
		return "";
	}

	// Parse the markdown document
	cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
	cmark_parser_feed(parser, markdown_str.c_str(), markdown_str.length());
	cmark_node *doc = cmark_parser_finish(parser);

	// Render as plain text
	char *text = cmark_render_plaintext(doc, CMARK_OPT_DEFAULT, 0);

	std::string result(text);

	// Clean up
	free(text);
	cmark_node_free(doc);
	cmark_parser_free(parser);

	return result;
}

MarkdownMetadata ExtractMetadata(const std::string &markdown_str) {
	MarkdownMetadata metadata;

	// Check for YAML frontmatter (use default flags, multiline not supported on Windows)
	std::regex frontmatter_regex(R"(^---\n([\s\S]*?)\n---)");
	std::smatch match;

	if (std::regex_search(markdown_str, match, frontmatter_regex)) {
		std::string yaml_content = match[1].str();

		// Basic YAML parsing for common fields (placeholder)
		std::istringstream stream(yaml_content);
		std::string line;
		while (std::getline(stream, line)) {
			auto colon_pos = line.find(':');
			if (colon_pos != std::string::npos) {
				std::string key = line.substr(0, colon_pos);
				std::string value = line.substr(colon_pos + 1);
				StringUtil::Trim(key);
				StringUtil::Trim(value);

				// Remove quotes if present
				if (value.front() == '"' && value.back() == '"') {
					value = value.substr(1, value.length() - 2);
				}

				// Store all fields in custom_fields map
				// No special handling - let users access any field uniformly
				metadata.custom_fields[key] = value;
			}
		}
	}

	return metadata;
}

std::string ExtractRawFrontmatter(const std::string &markdown_str) {
	// Check for YAML frontmatter
	std::regex frontmatter_regex(R"(^---\n([\s\S]*?)\n---)");
	std::smatch match;

	if (std::regex_search(markdown_str, match, frontmatter_regex)) {
		return match[1].str();
	}

	return "";
}

std::string StripFrontmatter(const std::string &markdown_str) {
	// Match frontmatter block including trailing newlines
	std::regex frontmatter_regex(R"(^---\n[\s\S]*?\n---\n*)");

	// Remove frontmatter and return the rest
	return std::regex_replace(markdown_str, frontmatter_regex, "");
}

Value MetadataToMap(const MarkdownMetadata &metadata) {
	// Build lists of keys and values for MAP construction
	vector<Value> keys;
	vector<Value> values;

	for (const auto &field : metadata.custom_fields) {
		keys.push_back(Value(field.first));
		values.push_back(Value(field.second));
	}

	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values));
}

MarkdownStats CalculateStats(const std::string &markdown_str) {
	MarkdownStats stats = {};

	// Word count (approximate)
	std::istringstream stream(markdown_str);
	std::string word;
	while (stream >> word) {
		stats.word_count++;
	}

	stats.char_count = markdown_str.length();
	stats.line_count = static_cast<idx_t>(std::count(markdown_str.begin(), markdown_str.end(), '\n')) + 1;

	// Count headings
	std::regex heading_regex(R"(^#{1,6}\s+)");
	stats.heading_count = static_cast<idx_t>(std::distance(
	    std::sregex_iterator(markdown_str.begin(), markdown_str.end(), heading_regex), std::sregex_iterator()));

	// Count code blocks
	std::regex code_block_regex(R"(```)");
	auto code_matches = std::distance(std::sregex_iterator(markdown_str.begin(), markdown_str.end(), code_block_regex),
	                                  std::sregex_iterator());
	stats.code_block_count = static_cast<idx_t>(code_matches) / 2; // Opening and closing

	// Count links
	std::regex link_regex(R"(\[([^\]]+)\]\([^)]+\))");
	stats.link_count = static_cast<idx_t>(std::distance(
	    std::sregex_iterator(markdown_str.begin(), markdown_str.end(), link_regex), std::sregex_iterator()));

	// Estimate reading time (200 words per minute average)
	stats.reading_time_minutes = static_cast<double>(stats.word_count) / 200.0;

	return stats;
}

//===--------------------------------------------------------------------===//
// Section Parsing
//===--------------------------------------------------------------------===//

std::string GenerateSectionId(const std::string &heading_text,
                              const std::unordered_map<std::string, int32_t> &id_counts) {
	// Generate GitHub-style anchor IDs
	std::string id = heading_text;

	// Convert to lowercase
	std::transform(id.begin(), id.end(), id.begin(), ::tolower);

	// Replace spaces and special chars with hyphens
	id = std::regex_replace(id, std::regex(R"([^a-z0-9\-_])"), "-");

	// Remove multiple consecutive hyphens
	id = std::regex_replace(id, std::regex(R"(-+)"), "-");

	// Trim leading/trailing hyphens
	id = std::regex_replace(id, std::regex(R"(^-+|-+$)"), "");

	// Handle duplicates
	auto it = id_counts.find(id);
	if (it != id_counts.end() && it->second > 0) {
		id += "-" + std::to_string(it->second);
	}

	return id;
}

std::vector<MarkdownSection> ParseSections(const std::string &markdown_str, int32_t min_level, int32_t max_level,
                                           bool include_content, const std::string &content_mode,
                                           idx_t max_content_length) {
	// Use the new cmark-based ExtractSections function instead of regex parsing
	return ExtractSections(markdown_str, min_level, max_level, include_content, content_mode, max_content_length);
}

std::vector<MarkdownSection> ExtractHeadings(const std::string &markdown_str, int32_t max_level) {
	return ParseSections(markdown_str, 1, max_level, false, "minimal", 0);
}

std::string ExtractSection(const std::string &markdown_str, const std::string &section_id, bool include_subsections) {
	// Parse sections with content using appropriate mode
	const std::string mode = include_subsections ? "full" : "minimal";
	auto sections = ParseSections(markdown_str, 1, 6, true, mode, 0);

	for (const auto &section : sections) {
		if (section.id == section_id) {
			return section.content;
		}
	}

	return ""; // Section not found
}

//===--------------------------------------------------------------------===//
// Content Extraction
//===--------------------------------------------------------------------===//

std::vector<CodeBlock> ExtractCodeBlocks(const std::string &markdown_str, const std::string &language_filter) {
	std::vector<CodeBlock> code_blocks;

	if (markdown_str.empty()) {
		return code_blocks;
	}

	// Parse with cmark-gfm
	cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
	cmark_parser_feed(parser, markdown_str.c_str(), markdown_str.length());
	cmark_node *doc = cmark_parser_finish(parser);

	// Walk the AST looking for code block nodes
	cmark_iter *iter = cmark_iter_new(doc);
	cmark_event_type ev_type;

	while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
		cmark_node *cur = cmark_iter_get_node(iter);

		if (ev_type == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_CODE_BLOCK) {
			CodeBlock block;

			// Get code block properties
			const char *info = cmark_node_get_fence_info(cur);
			const char *literal = cmark_node_get_literal(cur);

			block.info_string = info ? info : "";
			block.code = literal ? literal : "";
			block.line_number = cmark_node_get_start_line(cur);

			// Extract language from info string (first word)
			std::string language;
			if (!block.info_string.empty()) {
				size_t space_pos = block.info_string.find(' ');
				if (space_pos != std::string::npos) {
					language = block.info_string.substr(0, space_pos);
				} else {
					language = block.info_string;
				}

				// Trim whitespace
				StringUtil::Trim(language);
			}
			block.language = language;

			// Apply language filter if specified
			if (language_filter.empty() || StringUtil::Lower(block.language) == StringUtil::Lower(language_filter)) {
				code_blocks.push_back(block);
			}
		}
	}

	// Cleanup
	cmark_iter_free(iter);
	cmark_node_free(doc);
	cmark_parser_free(parser);

	return code_blocks;
}

std::vector<MarkdownSection> ExtractSections(const std::string &markdown_str, int32_t min_level, int32_t max_level,
                                             bool include_content, const std::string &content_mode,
                                             idx_t max_content_length) {
	std::vector<MarkdownSection> sections;
	std::unordered_map<std::string, int32_t> id_counts;

	if (markdown_str.empty()) {
		return sections;
	}

	// Strip frontmatter before parsing - cmark-gfm interprets --- as setext heading
	std::string content = StripFrontmatter(markdown_str);

	// Default max_content_length for smart mode
	idx_t effective_max_length = max_content_length > 0 ? max_content_length : 2000;

	// RAII wrapper for cmark resources
	struct CMarkRAII {
		cmark_parser *parser = nullptr;
		cmark_node *doc = nullptr;
		cmark_iter *iter = nullptr;

		CMarkRAII() {
			parser = cmark_parser_new(CMARK_OPT_DEFAULT);
			if (!parser)
				throw std::runtime_error("Failed to create cmark parser");
		}

		~CMarkRAII() {
			if (iter)
				cmark_iter_free(iter);
			if (doc)
				cmark_node_free(doc);
			if (parser)
				cmark_parser_free(parser);
		}

		// Delete copy constructor and assignment
		CMarkRAII(const CMarkRAII &) = delete;
		CMarkRAII &operator=(const CMarkRAII &) = delete;
	};

	CMarkRAII cmark;

	// Parse with cmark-gfm (using content with frontmatter stripped)
	cmark_parser_feed(cmark.parser, content.c_str(), content.length());
	cmark.doc = cmark_parser_finish(cmark.parser);
	if (!cmark.doc) {
		throw std::runtime_error("Failed to parse markdown document");
	}

	// Walk the AST looking for heading nodes
	cmark.iter = cmark_iter_new(cmark.doc);
	if (!cmark.iter) {
		throw std::runtime_error("Failed to create cmark iterator");
	}

	cmark_event_type ev_type;
	std::vector<cmark_node *> heading_nodes;
	std::vector<int32_t> heading_levels; // Track levels for all headings

	// First pass: collect all heading nodes (including those outside min/max range for reference)
	while ((ev_type = cmark_iter_next(cmark.iter)) != CMARK_EVENT_DONE) {
		cmark_node *cur = cmark_iter_get_node(cmark.iter);

		if (ev_type == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_HEADING) {
			int32_t level = cmark_node_get_heading_level(cur);
			heading_nodes.push_back(cur);
			heading_levels.push_back(level);
		}
	}

	// Second pass: process headings and extract content
	for (size_t i = 0; i < heading_nodes.size(); ++i) {
		cmark_node *heading = heading_nodes[i];
		int32_t level = heading_levels[i];

		// Skip headings outside the requested range
		if (level < min_level || level > max_level) {
			continue;
		}

		MarkdownSection section;

		// Get heading properties
		section.level = level;
		section.start_line = cmark_node_get_start_line(heading);
		section.end_line = cmark_node_get_end_line(heading);

		// Extract heading text by rendering to plain text
		// This handles all inline elements: code, emphasis, strong, links, etc.
		char *rendered_title = cmark_render_plaintext(heading, CMARK_OPT_DEFAULT, 0);
		std::string title_text;
		if (rendered_title) {
			title_text = rendered_title;
			free(rendered_title);
			// Remove trailing newline that cmark adds
			while (!title_text.empty() && (title_text.back() == '\n' || title_text.back() == '\r')) {
				title_text.pop_back();
			}
		}

		section.title = title_text;

		// Generate stable ID
		std::string base_id = GenerateSectionId(section.title, id_counts);
		id_counts[base_id]++;
		section.id = id_counts[base_id] > 1 ? base_id + "-" + std::to_string(id_counts[base_id] - 1) : base_id;

		// Find parent section and build section path
		section.parent_id = "";
		section.section_path = section.id;
		for (int j = static_cast<int>(sections.size()) - 1; j >= 0; --j) {
			if (sections[j].level < section.level) {
				section.parent_id = sections[j].id;
				section.section_path = sections[j].section_path + "/" + section.id;
				break;
			}
		}

		// Extract content if requested
		if (include_content) {
			// Find the stopping point based on content_mode
			cmark_node *stop_node = nullptr;
			idx_t stop_line = 0;

			for (size_t j = i + 1; j < heading_nodes.size(); ++j) {
				int32_t next_level = heading_levels[j];

				if (content_mode == "minimal") {
					// Stop at ANY next heading
					stop_node = heading_nodes[j];
					stop_line = cmark_node_get_start_line(stop_node) - 1;
					break;
				} else {
					// "full" or "smart": stop at same-or-higher level heading
					if (next_level <= section.level) {
						stop_node = heading_nodes[j];
						stop_line = cmark_node_get_start_line(stop_node) - 1;
						break;
					}
				}
			}

			// Update end_line based on stop point
			if (stop_line > 0) {
				section.end_line = stop_line;
			}

			// Extract content by walking through nodes
			std::string content_text;
			std::string immediate_content; // Content before first subsection (for smart mode)
			std::vector<std::pair<std::string, idx_t>> subsection_refs; // (id, line_count) for smart mode
			bool found_subsection = false;
			cmark_node *current = cmark_node_next(heading);

			while (current) {
				cmark_node_type node_type = cmark_node_get_type(current);

				// Check stopping conditions based on mode
				if (node_type == CMARK_NODE_HEADING) {
					int32_t current_level = cmark_node_get_heading_level(current);

					if (content_mode == "minimal") {
						// Stop at any heading
						break;
					} else if (current_level <= section.level) {
						// Stop at same-or-higher level for full/smart
						break;
					} else if (content_mode == "smart" && !found_subsection) {
						// First subsection in smart mode - save immediate content
						immediate_content = content_text;
						found_subsection = true;
					}
				}

				if (current == stop_node) {
					break;
				}

				// Convert this node back to markdown text
				char *rendered = cmark_render_commonmark(current, CMARK_OPT_DEFAULT, 0);
				if (rendered) {
					content_text += rendered;
					free(rendered);
				}

				current = cmark_node_next(current);
			}

			// Apply smart mode truncation if needed
			if (content_mode == "smart" && content_text.length() > effective_max_length) {
				// Build smart content with subsection references
				std::string smart_content;

				if (!immediate_content.empty()) {
					smart_content = immediate_content;
				} else {
					// Truncate at max_length with indicator
					smart_content = content_text.substr(0, effective_max_length);
					// Find last complete word/line
					size_t last_newline = smart_content.rfind('\n');
					if (last_newline != std::string::npos && last_newline > effective_max_length / 2) {
						smart_content = smart_content.substr(0, last_newline);
					}
				}

				// Add subsection references
				for (size_t j = i + 1; j < heading_nodes.size(); ++j) {
					int32_t sub_level = heading_levels[j];
					if (sub_level <= section.level) {
						break; // Reached end of subsections
					}
					if (sub_level == section.level + 1) {
						// Direct child subsection
						char *sub_title = cmark_render_plaintext(heading_nodes[j], CMARK_OPT_DEFAULT, 0);
						if (sub_title) {
							std::string sub_title_str = sub_title;
							free(sub_title);
							while (!sub_title_str.empty() &&
							       (sub_title_str.back() == '\n' || sub_title_str.back() == '\r')) {
								sub_title_str.pop_back();
							}
							std::string sub_id = GenerateSectionId(sub_title_str, id_counts);
							smart_content += "\n... (see #" + sub_id + ")\n";
						}
					}
				}

				section.content = smart_content;
			} else {
				section.content = content_text;
			}
		}

		sections.push_back(section);
	}

	return sections;
}

//===--------------------------------------------------------------------===//
// Block-Level Document Parsing
//===--------------------------------------------------------------------===//

// Helper to render a node's content as plain text
static std::string RenderNodeContent(cmark_node *node) {
	char *text = cmark_render_plaintext(node, CMARK_OPT_DEFAULT, 0);
	if (!text)
		return "";
	std::string result(text);
	free(text);
	// Trim trailing newlines
	while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
		result.pop_back();
	}
	return result;
}

// Helper to get text content from inline children
static std::string GetInlineText(cmark_node *node) {
	std::string result;
	cmark_node *child = cmark_node_first_child(node);
	while (child) {
		cmark_node_type type = cmark_node_get_type(child);
		if (type == CMARK_NODE_TEXT || type == CMARK_NODE_CODE) {
			const char *literal = cmark_node_get_literal(child);
			if (literal)
				result += literal;
		} else if (type == CMARK_NODE_SOFTBREAK) {
			result += " ";
		} else if (type == CMARK_NODE_LINEBREAK) {
			result += "\n";
		} else {
			// Recursively get text from nested nodes
			result += GetInlineText(child);
		}
		child = cmark_node_next(child);
	}
	return result;
}

std::vector<MarkdownBlock> ParseBlocks(const std::string &markdown_str) {
	std::vector<MarkdownBlock> blocks;

	if (markdown_str.empty()) {
		return blocks;
	}

	int32_t block_order = 1;

	// Check for frontmatter first
	std::string frontmatter = ExtractRawFrontmatter(markdown_str);
	if (!frontmatter.empty()) {
		MarkdownBlock fm_block;
		fm_block.block_type = "frontmatter";
		fm_block.content = frontmatter;
		fm_block.level = 0;
		fm_block.encoding = "yaml";
		fm_block.block_order = block_order++;
		blocks.push_back(fm_block);
	}

	// Strip frontmatter before parsing with cmark
	std::string body = StripFrontmatter(markdown_str);

	// Parse with cmark-gfm (with extensions for tables)
	cmark_gfm_core_extensions_ensure_registered(); // Must be called before finding extensions
	cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);

	// Enable GFM extensions
	cmark_syntax_extension *table_ext = cmark_find_syntax_extension("table");
	if (table_ext) {
		cmark_parser_attach_syntax_extension(parser, table_ext);
	}

	cmark_parser_feed(parser, body.c_str(), body.length());
	cmark_node *doc = cmark_parser_finish(parser);
	cmark_parser_free(parser);

	if (!doc) {
		return blocks;
	}

	// Iterate through top-level children of the document
	cmark_node *child = cmark_node_first_child(doc);

	while (child) {
		cmark_node_type node_type = cmark_node_get_type(child);
		MarkdownBlock block;
		block.encoding = "text";
		block.level = -1; // -1 = not applicable (will be NULL in output)
		block.block_order = block_order++;

		switch (node_type) {
		case CMARK_NODE_HEADING: {
			block.block_type = "heading";
			block.level = cmark_node_get_heading_level(child);
			block.content = GetInlineText(child);

			// Generate ID from heading text
			std::unordered_map<std::string, int32_t> id_counts;
			std::string id = GenerateSectionId(block.content, id_counts);
			block.attributes["id"] = id;
			break;
		}

		case CMARK_NODE_PARAGRAPH: {
			block.block_type = "paragraph";
			// Render paragraph to markdown to preserve inline formatting
			char *md = cmark_render_commonmark(child, CMARK_OPT_DEFAULT, 0);
			if (md) {
				block.content = md;
				free(md);
				// Trim trailing newlines
				while (!block.content.empty() && (block.content.back() == '\n' || block.content.back() == '\r')) {
					block.content.pop_back();
				}
			}
			break;
		}

		case CMARK_NODE_CODE_BLOCK: {
			block.block_type = "code";
			const char *literal = cmark_node_get_literal(child);
			block.content = literal ? literal : "";
			// Trim trailing newline from code content
			while (!block.content.empty() && block.content.back() == '\n') {
				block.content.pop_back();
			}
			const char *info = cmark_node_get_fence_info(child);
			if (info && strlen(info) > 0) {
				// Parse language from info string (first word)
				std::string info_str(info);
				size_t space_pos = info_str.find(' ');
				if (space_pos != std::string::npos) {
					block.attributes["language"] = info_str.substr(0, space_pos);
					block.attributes["info_string"] = info_str;
				} else {
					block.attributes["language"] = info_str;
				}
			}
			break;
		}

		case CMARK_NODE_BLOCK_QUOTE: {
			block.block_type = "blockquote";
			block.level = 1; // Could calculate nesting depth
			// Render blockquote content without the > prefix
			char *md = cmark_render_commonmark(child, CMARK_OPT_DEFAULT, 0);
			if (md) {
				block.content = md;
				free(md);
				// Remove leading > and trim
				std::string result;
				std::istringstream iss(block.content);
				std::string line;
				while (std::getline(iss, line)) {
					// Remove leading "> " or ">"
					if (line.length() >= 2 && line[0] == '>' && line[1] == ' ') {
						result += line.substr(2) + "\n";
					} else if (line.length() >= 1 && line[0] == '>') {
						result += line.substr(1) + "\n";
					} else {
						result += line + "\n";
					}
				}
				// Trim trailing newlines
				while (!result.empty() && result.back() == '\n') {
					result.pop_back();
				}
				block.content = result;
			}
			break;
		}

		case CMARK_NODE_LIST: {
			block.block_type = "list";
			block.level = 1;
			block.encoding = "json";

			cmark_list_type list_type = cmark_node_get_list_type(child);
			block.attributes["ordered"] = (list_type == CMARK_ORDERED_LIST) ? "true" : "false";

			if (list_type == CMARK_ORDERED_LIST) {
				int start = cmark_node_get_list_start(child);
				block.attributes["start"] = std::to_string(start);
			}

			// Build JSON array of list items
			std::string json = "[";
			bool first = true;
			cmark_node *item = cmark_node_first_child(child);
			while (item) {
				if (cmark_node_get_type(item) == CMARK_NODE_ITEM) {
					if (!first)
						json += ", ";
					first = false;

					// Get text content of list item (from first paragraph child)
					std::string item_text;
					cmark_node *item_child = cmark_node_first_child(item);
					while (item_child) {
						if (cmark_node_get_type(item_child) == CMARK_NODE_PARAGRAPH) {
							item_text = GetInlineText(item_child);
							break;
						} else if (cmark_node_get_type(item_child) == CMARK_NODE_LIST) {
							// Nested list - skip for now, just get first text
							break;
						} else {
							// Try to get inline text directly
							std::string txt = GetInlineText(item_child);
							if (!txt.empty()) {
								item_text = txt;
								break;
							}
						}
						item_child = cmark_node_next(item_child);
					}

					// Escape JSON special characters
					std::string escaped;
					for (char c : item_text) {
						switch (c) {
						case '"':
							escaped += "\\\"";
							break;
						case '\\':
							escaped += "\\\\";
							break;
						case '\n':
							escaped += "\\n";
							break;
						case '\r':
							escaped += "\\r";
							break;
						case '\t':
							escaped += "\\t";
							break;
						default:
							escaped += c;
						}
					}
					json += "\"" + escaped + "\"";
				}
				item = cmark_node_next(item);
			}
			json += "]";
			block.content = json;
			break;
		}

		case CMARK_NODE_THEMATIC_BREAK: {
			block.block_type = "hr";
			block.content = "";
			break;
		}

		case CMARK_NODE_HTML_BLOCK: {
			block.block_type = "html";
			const char *literal = cmark_node_get_literal(child);
			block.content = literal ? literal : "";
			// Trim trailing newlines
			while (!block.content.empty() && block.content.back() == '\n') {
				block.content.pop_back();
			}
			break;
		}

		default: {
			// Handle table extension and other nodes
			const char *type_string = cmark_node_get_type_string(child);
			if (type_string && strcmp(type_string, "table") == 0) {
				block.block_type = "table";
				block.encoding = "json";

				// Build JSON representation of table
				std::string headers_json = "";
				std::string rows_json = "";
				bool is_first_row = true;
				bool first_data_row = true;

				// Get table rows - in GFM tables, first row is always the header
				// The table may have table_header and table_row children
				cmark_node *row = cmark_node_first_child(child);

				while (row) {
					const char *row_type = cmark_node_get_type_string(row);
					// Accept both table_row and table_header node types
					bool is_row_node =
					    row_type && (strcmp(row_type, "table_row") == 0 || strcmp(row_type, "table_header") == 0);
					if (is_row_node) {
						std::string row_json = "[";
						bool first_cell = true;

						cmark_node *cell = cmark_node_first_child(row);
						while (cell) {
							const char *cell_type = cmark_node_get_type_string(cell);
							// Accept table_cell and any header cell variants
							bool is_cell = cell_type && (strcmp(cell_type, "table_cell") == 0 ||
							                             strstr(cell_type, "cell") != nullptr);
							if (is_cell) {
								if (!first_cell)
									row_json += ", ";
								first_cell = false;

								std::string cell_text = GetInlineText(cell);
								// Escape JSON
								std::string escaped;
								for (char c : cell_text) {
									switch (c) {
									case '"':
										escaped += "\\\"";
										break;
									case '\\':
										escaped += "\\\\";
										break;
									case '\n':
										escaped += "\\n";
										break;
									default:
										escaped += c;
									}
								}
								row_json += "\"" + escaped + "\"";
							}
							cell = cmark_node_next(cell);
						}
						row_json += "]";

						// First row is always the header in GFM tables
						if (is_first_row) {
							headers_json = row_json;
							is_first_row = false;
						} else {
							if (!first_data_row)
								rows_json += ", ";
							first_data_row = false;
							rows_json += row_json;
						}
					}
					row = cmark_node_next(row);
				}

				std::string json = "{\"headers\": " + headers_json + ", \"rows\": [" + rows_json + "]}";
				block.content = json;
			} else {
				// Unknown block type - render as raw
				block.block_type = "raw";
				char *md = cmark_render_commonmark(child, CMARK_OPT_DEFAULT, 0);
				if (md) {
					block.content = md;
					free(md);
				}
				if (type_string) {
					block.attributes["original_type"] = type_string;
				}
			}
			break;
		}
		}

		blocks.push_back(block);
		child = cmark_node_next(child);
	}

	cmark_node_free(doc);
	return blocks;
}

std::vector<MarkdownLink> ExtractLinks(const std::string &markdown_str) {
	std::vector<MarkdownLink> links;

	if (markdown_str.empty()) {
		return links;
	}

	// Pre-scan for reference link definitions to detect reference-style links
	// Reference definitions look like: [id]: url "optional title"
	// Pattern matches: [id]: followed by URL (optionally in angle brackets)
	// Process line-by-line for MSVC compatibility (no std::regex::multiline)
	std::set<std::string> reference_urls;
	std::regex ref_pattern(R"(^\s*\[([^\]]+)\]:\s+<?([^\s>]+)>?)");
	std::istringstream stream(markdown_str);
	std::string line;
	while (std::getline(stream, line)) {
		std::smatch match;
		if (std::regex_search(line, match, ref_pattern)) {
			std::string url = match[2].str();
			reference_urls.insert(url);
		}
	}

	// Parse with cmark-gfm
	cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
	cmark_parser_feed(parser, markdown_str.c_str(), markdown_str.length());
	cmark_node *doc = cmark_parser_finish(parser);

	// Walk the AST looking for link nodes
	cmark_iter *iter = cmark_iter_new(doc);
	cmark_event_type ev_type;

	while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
		cmark_node *cur = cmark_iter_get_node(iter);

		if (ev_type == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_LINK) {
			MarkdownLink link;

			// Get link properties
			const char *url = cmark_node_get_url(cur);
			const char *title = cmark_node_get_title(cur);

			link.url = url ? url : "";
			link.title = title ? title : "";
			link.line_number = cmark_node_get_start_line(cur);

			// Check if this URL matches a reference definition
			link.is_reference = reference_urls.find(link.url) != reference_urls.end();

			// Get link text from child text nodes
			cmark_node *child = cmark_node_first_child(cur);
			std::string text;
			while (child) {
				if (cmark_node_get_type(child) == CMARK_NODE_TEXT) {
					const char *child_text = cmark_node_get_literal(child);
					if (child_text)
						text += child_text;
				} else if (cmark_node_get_type(child) == CMARK_NODE_CODE) {
					// Handle inline code within links
					const char *code_text = cmark_node_get_literal(child);
					if (code_text)
						text += code_text;
				}
				child = cmark_node_next(child);
			}
			link.text = text;

			links.push_back(link);
		}
	}

	// Cleanup
	cmark_iter_free(iter);
	cmark_node_free(doc);
	cmark_parser_free(parser);

	return links;
}

std::vector<MarkdownImage> ExtractImages(const std::string &markdown_str) {
	std::vector<MarkdownImage> images;

	if (markdown_str.empty()) {
		return images;
	}

	// Parse with cmark-gfm
	cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
	cmark_parser_feed(parser, markdown_str.c_str(), markdown_str.length());
	cmark_node *doc = cmark_parser_finish(parser);

	// Walk the AST looking for image nodes
	cmark_iter *iter = cmark_iter_new(doc);
	cmark_event_type ev_type;

	while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
		cmark_node *cur = cmark_iter_get_node(iter);

		if (ev_type == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_IMAGE) {
			MarkdownImage image;

			// Get image properties
			const char *url = cmark_node_get_url(cur);
			const char *title = cmark_node_get_title(cur);

			image.url = url ? url : "";
			image.title = title ? title : "";
			image.line_number = cmark_node_get_start_line(cur); // Native line tracking!

			// Get alt text from child text nodes
			cmark_node *child = cmark_node_first_child(cur);
			std::string alt_text;
			while (child) {
				if (cmark_node_get_type(child) == CMARK_NODE_TEXT) {
					const char *text = cmark_node_get_literal(child);
					if (text)
						alt_text += text;
				}
				child = cmark_node_next(child);
			}
			image.alt_text = alt_text;

			images.push_back(image);
		}
	}

	// Cleanup
	cmark_iter_free(iter);
	cmark_node_free(doc);
	cmark_parser_free(parser);

	return images;
}

std::vector<MarkdownTable> ExtractTables(const std::string &markdown_str) {
	std::vector<MarkdownTable> tables;

	if (markdown_str.empty()) {
		return tables;
	}

	// For now, use a simpler regex-based approach for tables since the cmark-gfm table
	// extension node types are not easily accessible. We can revisit this later.

	// Parse tables using simple regex - this handles basic GFM tables
	std::regex table_regex(R"((?:^|\n)((?:\|[^\n]*\|[ \t]*\n?)+))");
	std::sregex_iterator iter(markdown_str.begin(), markdown_str.end(), table_regex);
	std::sregex_iterator end;

	for (; iter != end; ++iter) {
		const std::smatch &match = *iter;
		std::string table_content = match[1].str();

		// Calculate line number
		auto match_pos = match.position();
		idx_t line_number = 1 + std::count(markdown_str.begin(), markdown_str.begin() + match_pos, '\n');

		// Split into lines
		std::istringstream table_stream(table_content);
		std::string line;
		std::vector<std::string> table_lines;

		while (std::getline(table_stream, line)) {
			StringUtil::Trim(line);
			if (!line.empty()) {
				table_lines.push_back(line);
			}
		}

		if (table_lines.size() < 2) {
			continue; // Need at least header and separator
		}

		MarkdownTable table;
		table.line_number = line_number;

		// Find header row (first non-separator line)
		size_t header_idx = 0;

		// Skip separator lines (lines with only |, -, :, and whitespace)
		std::regex separator_regex(R"(^\s*\|?\s*[-|:\s]+\s*\|?\s*$)");
		while (header_idx < table_lines.size() && std::regex_match(table_lines[header_idx], separator_regex)) {
			header_idx++;
		}

		if (header_idx >= table_lines.size()) {
			continue; // No valid header found
		}

		// Additional check: ensure this is the actual header row by checking if next line is separator
		if (header_idx + 1 < table_lines.size() && !std::regex_match(table_lines[header_idx + 1], separator_regex)) {
			// This might not be a proper markdown table format, but let's try anyway
		}

		// Parse header row
		std::string header_line = table_lines[header_idx];
		// DEBUG: Output table parsing info to stderr (will show in build log)
		// fprintf(stderr, "DEBUG: Header idx=%zu, line='%s'\n", header_idx, header_line.c_str());
		if (header_line.front() == '|')
			header_line = header_line.substr(1);
		if (header_line.back() == '|')
			header_line = header_line.substr(0, header_line.length() - 1);

		std::regex cell_regex(R"([^|]+)");
		std::sregex_iterator cell_iter(header_line.begin(), header_line.end(), cell_regex);
		std::sregex_iterator cell_end;

		for (; cell_iter != cell_end; ++cell_iter) {
			std::string cell = (*cell_iter).str();
			StringUtil::Trim(cell);
			table.headers.push_back(cell);
		}

		table.num_columns = table.headers.size();

		// Find data rows (skip separator lines)
		for (size_t i = header_idx + 1; i < table_lines.size(); i++) {
			// Skip separator lines
			if (std::regex_match(table_lines[i], separator_regex)) {
				continue;
			}
			std::string data_line = table_lines[i];
			if (data_line.front() == '|')
				data_line = data_line.substr(1);
			if (data_line.back() == '|')
				data_line = data_line.substr(0, data_line.length() - 1);

			std::vector<std::string> row_data;
			std::sregex_iterator data_cell_iter(data_line.begin(), data_line.end(), cell_regex);

			for (; data_cell_iter != cell_end; ++data_cell_iter) {
				std::string cell = (*data_cell_iter).str();
				StringUtil::Trim(cell);
				row_data.push_back(cell);
			}

			// Pad row to match header column count
			while (row_data.size() < table.num_columns) {
				row_data.push_back("");
			}

			table.rows.push_back(row_data);
		}

		table.num_rows = table.rows.size();

		// Default to left alignment for now
		table.alignments.resize(table.num_columns, "left");

		tables.push_back(table);
	}

	return tables;
}

//===--------------------------------------------------------------------===//
// Utility Functions
//===--------------------------------------------------------------------===//

std::string GenerateBreadcrumb(const std::string &markdown_content, const std::string &section_id,
                               const std::string &separator) {
	// Parse sections from markdown
	auto sections = ExtractSections(markdown_content, 1, 6, true);

	if (sections.empty()) {
		return "";
	}

	// Build a map of section_id -> section for quick lookup
	std::map<std::string, const MarkdownSection *> section_map;
	for (const auto &section : sections) {
		section_map[section.id] = &section;
	}

	// Find the target section
	auto it = section_map.find(section_id);
	if (it == section_map.end()) {
		return ""; // Section not found
	}

	// Walk up the parent chain to collect titles
	std::vector<std::string> titles;
	const MarkdownSection *current = it->second;

	while (current != nullptr) {
		titles.push_back(current->title);
		if (current->parent_id.empty()) {
			break;
		}
		auto parent_it = section_map.find(current->parent_id);
		current = (parent_it != section_map.end()) ? parent_it->second : nullptr;
	}

	// Reverse to get root-to-leaf order
	std::reverse(titles.begin(), titles.end());

	// Join with separator
	std::string result;
	for (size_t i = 0; i < titles.size(); i++) {
		if (i > 0) {
			result += separator;
		}
		result += titles[i];
	}

	return result;
}

bool ValidateInternalLink(const std::string &markdown_str, const std::string &link_target) {
	if (!StringUtil::StartsWith(link_target, "#")) {
		return true; // External link, assume valid
	}

	std::string section_id = link_target.substr(1);
	auto sections = ExtractHeadings(markdown_str);

	for (const auto &section : sections) {
		if (section.id == section_id) {
			return true;
		}
	}

	return false;
}

std::string NormalizeMarkdown(const std::string &markdown_str) {
	// Basic normalization - could be more sophisticated
	std::string normalized = markdown_str;

	// Normalize line endings using StringUtil
	StringUtil::Replace(normalized, "\r\n", "\n");
	StringUtil::Replace(normalized, "\r", "\n");

	// Note: Skip trailing whitespace removal for now as it requires regex
	// This can be implemented later if needed

	return normalized;
}

} // namespace markdown_utils

} // namespace duckdb
