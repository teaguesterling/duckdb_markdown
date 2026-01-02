#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

/**
 * @brief Duck Block conversion functions for converting doc_block types to Markdown
 *
 * This class provides functions for:
 * - Converting a single doc_block to Markdown (duck_block_to_md)
 * - Converting a list of doc_blocks to Markdown (duck_blocks_to_md)
 * - Converting doc_blocks to markdown_sections format (duck_blocks_to_sections)
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
	 * @brief Render a single doc_block to Markdown string
	 *
	 * @param block_type The type of block (heading, paragraph, code, etc.)
	 * @param content The block content
	 * @param level The heading level or nesting depth
	 * @param encoding The content encoding (text, json, yaml, html, xml, inline)
	 * @param attributes The block attributes as a MAP value
	 * @return The rendered Markdown string
	 */
	static string RenderBlockToMarkdown(const string &block_type, const string &content, int32_t level,
	                                    const string &encoding, const Value &attributes);

	/**
	 * @brief Render a single doc_element to Markdown string
	 *
	 * @param kind 'block' or 'inline'
	 * @param element_type The type (heading, paragraph, bold, link, etc.)
	 * @param content The element content
	 * @param level The level (heading level, nesting depth)
	 * @param encoding The content encoding
	 * @param attributes The element attributes as a MAP value
	 * @return The rendered Markdown string
	 */
	static string RenderElementToMarkdown(const string &kind, const string &element_type, const string &content,
	                                      int32_t level, const string &encoding, const Value &attributes);

	/**
	 * @brief Render a list of doc_element structs to Markdown string
	 *
	 * @param elements_value A LIST of doc_element structs
	 * @return The concatenated Markdown string
	 */
	static string RenderElementsToMarkdown(const Value &elements_value);

private:
	static void RegisterBlockToMdFunction(ExtensionLoader &loader);
	static void RegisterBlocksToMdFunction(ExtensionLoader &loader);
	static void RegisterBlocksToSectionsFunction(ExtensionLoader &loader);
	static void RegisterElementToMdFunction(ExtensionLoader &loader);
	static void RegisterElementsToMdFunction(ExtensionLoader &loader);

	// Helper for inline element rendering (used by RenderElementToMarkdown)
	static string RenderInlineElementToMarkdown(const string &element_type, const string &content,
	                                            const Value &attributes);

	// Helper to extract attribute from MAP value
	static string GetAttribute(const Value &attributes, const string &key);

	// Helper to parse JSON list items
	static vector<string> ParseJsonListItems(const string &content);

	// Helper to parse JSON table
	static void ParseJsonTable(const string &content, vector<string> &headers, vector<vector<string>> &rows);
};

} // namespace duckdb
