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
    
    // Check for YAML frontmatter
    std::regex frontmatter_regex(R"(^---\n([\s\S]*?)\n---)", std::regex_constants::multiline);
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
    std::regex heading_regex(R"(^#{1,6}\s+)", std::regex_constants::multiline);
    stats.heading_count = static_cast<idx_t>(std::distance(
        std::sregex_iterator(markdown_str.begin(), markdown_str.end(), heading_regex),
        std::sregex_iterator()
    ));
    
    // Count code blocks
    std::regex code_block_regex(R"(```)", std::regex_constants::multiline);
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
    std::vector<MarkdownSection> sections;
    std::unordered_map<std::string, int32_t> id_counts;
    
    // Parse headings and create sections
    std::regex heading_regex(R"(^(#{1,6})\s+(.+)$)", std::regex_constants::multiline);
    std::sregex_iterator iter(markdown_str.begin(), markdown_str.end(), heading_regex);
    std::sregex_iterator end;
    
    std::vector<std::pair<int32_t, std::string>> heading_stack; // level, id
    std::vector<std::tuple<size_t, size_t, int32_t, std::string, std::string>> heading_positions; // start, end, level, title, id
    
    // First pass: collect all heading positions
    for (auto current_iter = iter; current_iter != end; ++current_iter) {
        const std::smatch& match = *current_iter;
        int32_t level = static_cast<int32_t>(match[1].length());
        std::string title = match[2].str();
        
        if (level < min_level || level > max_level) {
            continue;
        }
        
        // Generate stable ID
        std::string base_id = GenerateSectionId(title, id_counts);
        id_counts[base_id]++;
        std::string section_id = id_counts[base_id] > 1 ? 
            base_id + "-" + std::to_string(id_counts[base_id] - 1) : base_id;
        
        size_t start_pos = match.position();
        heading_positions.push_back({start_pos, 0, level, title, section_id});
    }
    
    // Set end positions for each heading (start of next heading at same or higher level)
    for (size_t i = 0; i < heading_positions.size(); ++i) {
        auto& [start_pos, end_pos, level, title, section_id] = heading_positions[i];
        
        end_pos = markdown_str.length(); // Default to end of document
        
        // Find next heading at same or higher level
        for (size_t j = i + 1; j < heading_positions.size(); ++j) {
            auto& [next_start, next_end, next_level, next_title, next_id] = heading_positions[j];
            if (next_level <= level) {
                end_pos = next_start;
                break;
            }
        }
    }
    
    // Reset for second pass
    id_counts.clear();
    
    // Second pass: create sections with content
    for (const auto& [start_pos, end_pos, level, title, _] : heading_positions) {
        // Regenerate ID (need to maintain same logic)
        std::string base_id = GenerateSectionId(title, id_counts);
        id_counts[base_id]++;
        std::string section_id = id_counts[base_id] > 1 ? 
            base_id + "-" + std::to_string(id_counts[base_id] - 1) : base_id;
        
        // Find parent
        std::string parent_id = "";
        idx_t position = 0;
        
        // Remove deeper levels from stack
        while (!heading_stack.empty() && heading_stack.back().first >= level) {
            heading_stack.pop_back();
        }
        
        if (!heading_stack.empty()) {
            parent_id = heading_stack.back().second;
            
            // Count siblings at this level
            for (const auto& section : sections) {
                if (section.parent_id == parent_id && section.level == level) {
                    position++;
                }
            }
        }
        
        heading_stack.push_back({level, section_id});
        
        MarkdownSection section;
        section.id = section_id;
        section.level = level;
        section.title = title;
        section.parent_id = parent_id;
        section.position = position;
        section.start_line = std::count(markdown_str.begin(), markdown_str.begin() + start_pos, '\n') + 1;
        section.end_line = std::count(markdown_str.begin(), markdown_str.begin() + end_pos, '\n') + 1;
        
        if (include_content) {
            // Extract section content from start_pos to end_pos
            section.content = markdown_str.substr(start_pos, end_pos - start_pos);
            
            // Clean up content - remove trailing whitespace and extra newlines
            section.content = std::regex_replace(section.content, std::regex(R"(\s+$)"), "");
        }
        
        sections.push_back(section);
    }
    
    return sections;
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
    
    // TODO: Extract fenced code blocks using simple string parsing
    // For now, return empty vector to avoid regex dependency
    // This will be implemented later using DuckDB string utilities
    (void)language_filter; // Avoid unused parameter warning
    
    return code_blocks;
}

std::vector<MarkdownLink> ExtractLinks(const std::string& markdown_str) {
    std::vector<MarkdownLink> links;
    
    // Extract inline links
    std::regex link_regex(R"(\[([^\]]+)\]\(([^)]+)\))");
    std::sregex_iterator iter(markdown_str.begin(), markdown_str.end(), link_regex);
    std::sregex_iterator end;
    
    for (; iter != end; ++iter) {
        const std::smatch& match = *iter;
        MarkdownLink link;
        link.text = match[1].str();
        link.url = match[2].str();
        link.is_reference = false;
        
        // Split URL and title if present
        auto space_pos = link.url.find(' ');
        if (space_pos != std::string::npos) {
            link.title = link.url.substr(space_pos + 1);
            link.url = link.url.substr(0, space_pos);
            
            // Remove quotes from title
            if (link.title.front() == '"' && link.title.back() == '"') {
                link.title = link.title.substr(1, link.title.length() - 2);
            }
        }
        
        links.push_back(link);
    }
    
    return links;
}

std::vector<MarkdownImage> ExtractImages(const std::string& markdown_str) {
    std::vector<MarkdownImage> images;
    
    // Extract images (similar to links but with leading !)
    std::regex image_regex(R"(!\[([^\]]*)\]\(([^)]+)\))");
    std::sregex_iterator iter(markdown_str.begin(), markdown_str.end(), image_regex);
    std::sregex_iterator end;
    
    for (; iter != end; ++iter) {
        const std::smatch& match = *iter;
        MarkdownImage image;
        image.alt_text = match[1].str();
        image.url = match[2].str();
        
        // Split URL and title if present
        auto space_pos = image.url.find(' ');
        if (space_pos != std::string::npos) {
            image.title = image.url.substr(space_pos + 1);
            image.url = image.url.substr(0, space_pos);
            
            // Remove quotes from title
            if (image.title.front() == '"' && image.title.back() == '"') {
                image.title = image.title.substr(1, image.title.length() - 2);
            }
        }
        
        images.push_back(image);
    }
    
    return images;
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
