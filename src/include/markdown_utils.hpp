#pragma once

#include "duckdb.hpp"
#include <string>
#include <vector>

namespace duckdb {

namespace markdown_utils {

//===--------------------------------------------------------------------===//
// Markdown Flavor Settings
//===--------------------------------------------------------------------===//

enum class MarkdownFlavor {
    GFM,          // GitHub Flavored Markdown (default)
    COMMONMARK,   // Standard CommonMark
    MULTIMARKDOWN // Extended features
};

//===--------------------------------------------------------------------===//
// Section Structure
//===--------------------------------------------------------------------===//

struct MarkdownSection {
    std::string id;              // Stable section identifier
    int level;                   // Heading level (1-6)
    std::string title;           // Heading text
    std::string content;         // Section content (including subsections)
    std::string parent_id;       // Parent section ID (empty for top-level)
    idx_t position;                // Position within parent
    idx_t start_line;           // Starting line number
    idx_t end_line;             // Ending line number
};

struct MarkdownMetadata {
    std::string title;
    std::string description;
    std::vector<std::string> tags;
    std::string date;
    std::map<std::string, std::string> custom_fields;
};

struct MarkdownStats {
    idx_t word_count;
    idx_t char_count;
    idx_t line_count;
    idx_t heading_count;
    idx_t code_block_count;
    idx_t link_count;
    double reading_time_minutes;  // Estimated reading time
};

//===--------------------------------------------------------------------===//
// Core Conversion Functions
//===--------------------------------------------------------------------===//

// Convert Markdown to HTML
std::string MarkdownToHTML(const std::string& markdown_str, MarkdownFlavor flavor = MarkdownFlavor::GFM);

// Convert Markdown to plain text (for FTS)
std::string MarkdownToText(const std::string& markdown_str);

// Extract frontmatter metadata
MarkdownMetadata ExtractMetadata(const std::string& markdown_str);

// Extract raw frontmatter YAML content (without --- delimiters)
// Returns empty string if no frontmatter found
std::string ExtractRawFrontmatter(const std::string& markdown_str);

// Strip frontmatter from markdown content, returning only the body
// This is needed because cmark-gfm doesn't understand YAML frontmatter
std::string StripFrontmatter(const std::string& markdown_str);

// Convert metadata to DuckDB MAP value
Value MetadataToMap(const MarkdownMetadata& metadata);

// Calculate document statistics
MarkdownStats CalculateStats(const std::string& markdown_str);

//===--------------------------------------------------------------------===//
// Section Parsing
//===--------------------------------------------------------------------===//

// Parse document into sections
std::vector<MarkdownSection> ParseSections(const std::string& markdown_str, 
                                          int32_t min_level = 1, 
                                          int32_t max_level = 6,
                                          bool include_content = true);

// Generate stable section IDs
std::string GenerateSectionId(const std::string& heading_text, 
                             const std::unordered_map<std::string, int32_t>& id_counts);

// Extract specific section by ID
std::string ExtractSection(const std::string& markdown_str, const std::string& section_id);

//===--------------------------------------------------------------------===//
// Content Extraction
//===--------------------------------------------------------------------===//

struct CodeBlock {
    std::string language;
    std::string code;
    idx_t line_number;
    std::string info_string;  // Full info string after language
};

struct MarkdownLink {
    std::string text;
    std::string url;
    std::string title;
    bool is_reference;
    idx_t line_number;
};

struct MarkdownImage {
    std::string alt_text;
    std::string url;
    std::string title;
    idx_t line_number;
};

struct MarkdownTable {
    std::vector<std::string> headers;  // Table headers
    std::vector<std::string> alignments; // Column alignments (left, right, center)
    std::vector<std::vector<std::string>> rows; // Table data rows
    idx_t line_number;
    idx_t num_columns;
    idx_t num_rows;
};

// Extract code blocks
std::vector<CodeBlock> ExtractCodeBlocks(const std::string& markdown_str, 
                                        const std::string& language_filter = "");

// Extract sections using cmark-gfm AST (replacement for regex-based ParseSections)
std::vector<MarkdownSection> ExtractSections(const std::string& markdown_str, 
                                            int32_t min_level = 1, 
                                            int32_t max_level = 6,
                                            bool include_content = true);

// Extract links
std::vector<MarkdownLink> ExtractLinks(const std::string& markdown_str);

// Extract images
std::vector<MarkdownImage> ExtractImages(const std::string& markdown_str);

// Extract tables
std::vector<MarkdownTable> ExtractTables(const std::string& markdown_str);

// Extract headings for TOC
std::vector<MarkdownSection> ExtractHeadings(const std::string& markdown_str, int32_t max_level = 6);

//===--------------------------------------------------------------------===//
// Utility Functions
//===--------------------------------------------------------------------===//

// Generate breadcrumb path for a section
std::string GenerateBreadcrumb(const std::string& file_path, const std::string& section_id);

// Validate internal links
bool ValidateInternalLink(const std::string& markdown_str, const std::string& link_target);

// Normalize Markdown content
std::string NormalizeMarkdown(const std::string& markdown_str);

} // namespace markdown_utils

} // namespace duckdb
