#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

/**
 * @brief Duck Block conversion functions for converting duck_block types to Markdown
 *
 * This class provides functions for:
 * - Converting a single duck_block to Markdown (duck_block_to_md)
 * - Converting a list of duck_blocks to Markdown (duck_blocks_to_md)
 * - Converting duck_blocks to sections format (duck_blocks_to_sections)
 *
 * Duck block type (from duck_block_utils):
 *   STRUCT(kind, element_type, content, level, encoding, attributes, element_order)
 *
 * These functions enable round-trip document processing:
 *   read_markdown_blocks() -> manipulate -> duck_blocks_to_md()
 */
class DuckBlockFunctions {
public:
	/**
	 * @brief Register all duck_block functions with DuckDB
	 */
	static void Register(ExtensionLoader &loader);

	/**
	 * @brief Render a single duck_block to Markdown string
	 *
	 * @param kind 'block' or 'inline'
	 * @param element_type The type (heading, paragraph, bold, link, etc.)
	 * @param content The element content
	 * @param level The level (heading level via attribute, or nesting depth)
	 * @param encoding The content encoding (text, json, yaml, html, xml)
	 * @param attributes The element attributes as a MAP value
	 * @return The rendered Markdown string
	 */
	static string RenderDuckBlockToMarkdown(const string &kind, const string &element_type, const string &content,
	                                        int32_t level, const string &encoding, const Value &attributes);

	/**
	 * @brief Render a list of duck_block structs to Markdown string
	 *
	 * @param blocks_value A LIST of duck_block structs
	 * @return The concatenated Markdown string
	 */
	static string RenderDuckBlocksToMarkdown(const Value &blocks_value);

private:
	static void RegisterDuckBlockToMdFunction(ExtensionLoader &loader);
	static void RegisterDuckBlocksToMdFunction(ExtensionLoader &loader);
	static void RegisterDuckBlocksToSectionsFunction(ExtensionLoader &loader);

	// Render block-level element to markdown
	static string RenderBlockElementToMarkdown(const string &element_type, const string &content, int32_t level,
	                                           const string &encoding, const Value &attributes);

	// Render inline element to markdown
	static string RenderInlineElementToMarkdown(const string &element_type, const string &content,
	                                            const Value &attributes);

	// Helper to extract attribute from MAP value
	static string GetAttribute(const Value &attributes, const string &key);

	// Helper to parse JSON list items
	static vector<string> ParseJsonListItems(const string &content);

	// Helper to parse JSON table
	static void ParseJsonTable(const string &content, vector<string> &headers, vector<vector<string>> &rows);

	// Helper to extract plain text from Pandoc AST inline elements
	static string ExtractPandocText(const string &content);

	// Check if content looks like Pandoc table format
	static bool IsPandocTableFormat(const string &content);

	// Parse Pandoc table format into headers and rows
	static void ParsePandocTable(const string &content, vector<string> &headers, vector<vector<string>> &rows);
};

} // namespace duckdb
