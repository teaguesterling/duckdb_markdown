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

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
    // Register Markdown reader
    MarkdownReader::RegisterFunction(loader);

    // Register Markdown functions
    MarkdownFunctions::Register(loader);

    // Register Markdown extraction functions
    MarkdownExtractionFunctions::Register(loader);

    // Register Markdown types
    MarkdownTypes::Register(loader);

    // Register Markdown copy functions
    RegisterMarkdownCopyFunctions(loader);
}

void MarkdownExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);

    // Register Markdown files as automatically recognized by DuckDB
    auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

    // Add replacement scan for Markdown files
    config.replacement_scans.emplace_back(MarkdownReader::ReadMarkdownReplacement);
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
