#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/config.hpp"

#include "markdown_extension.hpp"
#include "markdown_reader.hpp"
#include "markdown_types.hpp"
#include "markdown_scalar_functions.hpp"
#include "markdown_extraction_functions.hpp"
#include "duck_block_functions.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Register Markdown reader
	MarkdownReader::RegisterFunction(loader);

	// Register Markdown functions
	MarkdownFunctions::Register(loader);

	// Register Markdown extraction functions
	MarkdownExtractionFunctions::Register(loader);

	// Register duck_block conversion functions
	DuckBlockFunctions::Register(loader);

	// Register Markdown types
	MarkdownTypes::Register(loader);

	// Register Markdown copy functions
	RegisterMarkdownCopyFunctions(loader);

	// Make bare `FROM 'file.md'` work, by registering the replacement scan.
	//
	// THIS BELONGS HERE, NOT IN MarkdownExtension::Load. There are two load paths
	// and they do not run the same code:
	//
	//   static build    -> MarkdownExtension::Load(loader), which calls LoadInternal
	//   loadable build  -> markdown_duckdb_cpp_init(loader) (DUCKDB_CPP_EXTENSION_ENTRY
	//                      at the bottom of this file), which calls LoadInternal ONLY
	//
	// While this registration sat in Load(), the loadable extension registered every
	// table function but no replacement scan, so `read_markdown('x.md')` worked and
	// `FROM 'x.md'` raised "No extension found that is capable of reading the file"
	// (#82). Both paths reach LoadInternal, so registering here covers both.
	//
	// NOT a DuckDB 2.0 regression, which is what #82 assumed: DUCKDB_CPP_EXTENSION_ENTRY
	// expands identically on v1.5.6 and v2.0-cyanoptera, and the failure reproduces on
	// a stock v1.5.6 CLI. Local and CI stable runs hid it by linking the extension
	// statically, which runs Load(); only a loadable load exposes it.
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(MarkdownReader::ReadMarkdownReplacement);
}

void MarkdownExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string MarkdownExtension::Name() {
	return "markdown";
}

std::string MarkdownExtension::Version() const {
#ifdef EXT_VERSION_MARKDOWN
	return EXT_VERSION_MARKDOWN;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(markdown, loader) {
	duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *markdown_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
