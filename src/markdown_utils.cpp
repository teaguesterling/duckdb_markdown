#include "markdown_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include <algorithm>
#include <regex>
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

std::string MarkdownToHTML(const std::string& markdown_str, MarkdownFlavor flavor) {
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
        
        if (table_extension) cmark_parser_attach_syntax_extension(parser, table_extension);
        if (strikethrough_extension) cmark_parser_attach_syntax_extension(parser, strikethrough_extension);
        if (autolink_extension) cmark_parser_attach_syntax_extension(parser, autolink_extension);
        if (tagfilter_extension) cmark_parser_attach_syntax_extension(parser, tagfilter_extension);
        if (tasklist_extension) cmark_parser_attach_syntax_extension(parser, tasklist_extension);
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

std::string MarkdownToText(const std::string& markdown_str) {
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

MarkdownMetadata ExtractMetadata(const std::string& markdown_str) {
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
                
                if (key == "title") {
                    metadata.title = value;
                } else if (key == "description") {
                    metadata.description = value;
                } else if (key == "date") {
                    metadata.date = value;
                } else if (key == "tags") {
                    // Basic tag parsing (should handle arrays properly)
                    metadata.tags.push_back(value);
                } else {
                    metadata.custom_fields[key] = value;
                }
            }
        }
    }
    
    return metadata;
}

MarkdownStats CalculateStats(const std::string& markdown_str) {
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
        std::sregex_iterator(markdown_str.begin(), markdown_str.end(), heading_regex),
        std::sregex_iterator()
    ));
    
    // Count code blocks
    std::regex code_block_regex(R"(```)");  
    auto code_matches = std::distance(
        std::sregex_iterator(markdown_str.begin(), markdown_str.end(), code_block_regex),
        std::sregex_iterator()
    );
    stats.code_block_count = static_cast<idx_t>(code_matches) / 2; // Opening and closing
    
    // Count links
    std::regex link_regex(R"(\[([^\]]+)\]\([^)]+\))");
    stats.link_count = static_cast<idx_t>(std::distance(
        std::sregex_iterator(markdown_str.begin(), markdown_str.end(), link_regex),
        std::sregex_iterator()
    ));
    
    // Estimate reading time (200 words per minute average)
    stats.reading_time_minutes = static_cast<double>(stats.word_count) / 200.0;
    
    return stats;
}

//===--------------------------------------------------------------------===//
// Section Parsing
//===--------------------------------------------------------------------===//

std::string GenerateSectionId(const std::string& heading_text, 
                             const std::unordered_map<std::string, int32_t>& id_counts) {
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

std::vector<MarkdownSection> ParseSections(const std::string& markdown_str, 
                                          int32_t min_level, 
                                          int32_t max_level,
                                          bool include_content) {
    // Use the new cmark-based ExtractSections function instead of regex parsing
    return ExtractSections(markdown_str, min_level, max_level, include_content);
}

std::vector<MarkdownSection> ExtractHeadings(const std::string& markdown_str, int32_t max_level) {
    return ParseSections(markdown_str, 1, max_level, false);
}

std::string ExtractSection(const std::string& markdown_str, const std::string& section_id) {
    // Parse sections with content
    auto sections = ParseSections(markdown_str, 1, 6, true);
    
    for (const auto& section : sections) {
        if (section.id == section_id) {
            return section.content;
        }
    }
    
    return ""; // Section not found
}

//===--------------------------------------------------------------------===//
// Content Extraction
//===--------------------------------------------------------------------===//

std::vector<CodeBlock> ExtractCodeBlocks(const std::string& markdown_str, 
                                        const std::string& language_filter) {
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
            if (language_filter.empty() || 
                StringUtil::Lower(block.language) == StringUtil::Lower(language_filter)) {
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

std::vector<MarkdownSection> ExtractSections(const std::string& markdown_str, 
                                            int32_t min_level, 
                                            int32_t max_level,
                                            bool include_content) {
    std::vector<MarkdownSection> sections;
    std::unordered_map<std::string, int32_t> id_counts;
    
    if (markdown_str.empty()) {
        return sections;
    }
    
    // RAII wrapper for cmark resources
    struct CMarkRAII {
        cmark_parser *parser = nullptr;
        cmark_node *doc = nullptr;
        cmark_iter *iter = nullptr;
        
        CMarkRAII() {
            parser = cmark_parser_new(CMARK_OPT_DEFAULT);
            if (!parser) throw std::runtime_error("Failed to create cmark parser");
        }
        
        ~CMarkRAII() {
            if (iter) cmark_iter_free(iter);
            if (doc) cmark_node_free(doc);
            if (parser) cmark_parser_free(parser);
        }
        
        // Delete copy constructor and assignment
        CMarkRAII(const CMarkRAII&) = delete;
        CMarkRAII& operator=(const CMarkRAII&) = delete;
    };
    
    CMarkRAII cmark;
    
    // Parse with cmark-gfm
    cmark_parser_feed(cmark.parser, markdown_str.c_str(), markdown_str.length());
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
    std::vector<cmark_node*> heading_nodes;
    
    // First pass: collect all heading nodes
    while ((ev_type = cmark_iter_next(cmark.iter)) != CMARK_EVENT_DONE) {
        cmark_node *cur = cmark_iter_get_node(cmark.iter);
        
        if (ev_type == CMARK_EVENT_ENTER && cmark_node_get_type(cur) == CMARK_NODE_HEADING) {
            int32_t level = cmark_node_get_heading_level(cur);
            
            if (level >= min_level && level <= max_level) {
                heading_nodes.push_back(cur);
            }
        }
    }
    
    // Second pass: process headings and extract content
    for (size_t i = 0; i < heading_nodes.size(); ++i) {
        cmark_node *heading = heading_nodes[i];
        MarkdownSection section;
        
        // Get heading properties
        section.level = cmark_node_get_heading_level(heading);
        section.start_line = cmark_node_get_start_line(heading);
        section.end_line = cmark_node_get_end_line(heading);
        
        // Extract heading text using RAII iterator
        struct TextIterRAII {
            cmark_iter *iter = nullptr;
            
            TextIterRAII(cmark_node *node) {
                iter = cmark_iter_new(node);
                if (!iter) throw std::runtime_error("Failed to create text iterator");
            }
            
            ~TextIterRAII() {
                if (iter) cmark_iter_free(iter);
            }
            
            TextIterRAII(const TextIterRAII&) = delete;
            TextIterRAII& operator=(const TextIterRAII&) = delete;
        };
        
        TextIterRAII text_iter(heading);
        std::string title_text;
        
        while ((ev_type = cmark_iter_next(text_iter.iter)) != CMARK_EVENT_DONE) {
            cmark_node *text_node = cmark_iter_get_node(text_iter.iter);
            if (cmark_node_get_type(text_node) == CMARK_NODE_TEXT) {
                const char *text = cmark_node_get_literal(text_node);
                if (text) {
                    title_text += text;
                }
            }
        }
        
        section.title = title_text;
        
        // Generate stable ID
        std::string base_id = GenerateSectionId(section.title, id_counts);
        id_counts[base_id]++;
        section.id = id_counts[base_id] > 1 ? 
            base_id + "-" + std::to_string(id_counts[base_id] - 1) : base_id;
        
        // Find parent section
        section.parent_id = "";
        for (int j = static_cast<int>(sections.size()) - 1; j >= 0; --j) {
            if (sections[j].level < section.level) {
                section.parent_id = sections[j].id;
                break;
            }
        }
        
        // Extract content if requested
        if (include_content) {
            // Find the end position for content extraction
            cmark_node *next_heading = nullptr;
            if (i + 1 < heading_nodes.size()) {
                next_heading = heading_nodes[i + 1];
                // Only use it if it's at the same or higher level
                if (cmark_node_get_heading_level(next_heading) <= section.level) {
                    section.end_line = cmark_node_get_start_line(next_heading) - 1;
                }
            }
            
            // Extract content by walking through all nodes between this heading and the next
            std::string content_text;
            cmark_node *current = cmark_node_next(heading);
            
            while (current && current != next_heading) {
                cmark_node_type node_type = cmark_node_get_type(current);
                
                // Skip headings at same or higher level
                if (node_type == CMARK_NODE_HEADING && 
                    cmark_node_get_heading_level(current) <= section.level) {
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
            
            section.content = content_text;
        }
        
        sections.push_back(section);
    }
    
    return sections;
}

std::vector<MarkdownLink> ExtractLinks(const std::string& markdown_str) {
    std::vector<MarkdownLink> links;
    
    if (markdown_str.empty()) {
        return links;
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
            link.is_reference = false; // TODO: Detect reference links
            
            // Get link text from child text nodes
            cmark_node *child = cmark_node_first_child(cur);
            std::string text;
            while (child) {
                if (cmark_node_get_type(child) == CMARK_NODE_TEXT) {
                    const char *child_text = cmark_node_get_literal(child);
                    if (child_text) text += child_text;
                } else if (cmark_node_get_type(child) == CMARK_NODE_CODE) {
                    // Handle inline code within links
                    const char *code_text = cmark_node_get_literal(child);
                    if (code_text) text += code_text;
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

std::vector<MarkdownImage> ExtractImages(const std::string& markdown_str) {
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
                    if (text) alt_text += text;
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

std::vector<MarkdownTable> ExtractTables(const std::string& markdown_str) {
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
        const std::smatch& match = *iter;
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
        if (header_line.front() == '|') header_line = header_line.substr(1);
        if (header_line.back() == '|') header_line = header_line.substr(0, header_line.length() - 1);
        
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
            if (data_line.front() == '|') data_line = data_line.substr(1);
            if (data_line.back() == '|') data_line = data_line.substr(0, data_line.length() - 1);
            
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

std::string GenerateBreadcrumb(const std::string& file_path, const std::string& section_id) {
    // TODO: Build full breadcrumb path by walking up parent sections
    return file_path + "#" + section_id;
}

bool ValidateInternalLink(const std::string& markdown_str, const std::string& link_target) {
    if (!StringUtil::StartsWith(link_target, "#")) {
        return true; // External link, assume valid
    }
    
    std::string section_id = link_target.substr(1);
    auto sections = ExtractHeadings(markdown_str);
    
    for (const auto& section : sections) {
        if (section.id == section_id) {
            return true;
        }
    }
    
    return false;
}

std::string NormalizeMarkdown(const std::string& markdown_str) {
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
