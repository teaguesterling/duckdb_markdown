#pragma once

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

/**
 * @brief Markdown scalar functions for converting and validating Markdown content
 *
 * This class provides scalar functions for:
 * - Converting Markdown to HTML (md_to_html)
 * - Converting Markdown to plain text (md_to_text)
 * - Converting values to Markdown (value_to_md)
 * - Validating Markdown syntax (md_valid)
 * - Extracting metadata (md_extract_metadata)
 * - Calculating statistics (md_stats)
 */
class MarkdownFunctions {
public:
	/**
	 * @brief Register all Markdown scalar functions with DuckDB
	 *
	 * @param loader The extension loader to register the functions with
	 */
	static void Register(ExtensionLoader &loader);

private:
	static void RegisterValidationFunction(ExtensionLoader &loader);
	static void RegisterConversionFunctions(ExtensionLoader &loader);
	static void RegisterMarkdownTypeFunctions(ExtensionLoader &loader);
	static void RegisterStatsFunctions(ExtensionLoader &loader);
	static void RegisterMetadataFunctions(ExtensionLoader &loader);
};

} // namespace duckdb
