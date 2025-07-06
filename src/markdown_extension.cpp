#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/config.hpp"

#include "markdown_extension.hpp"
#include "markdown_reader.hpp"
#include "markdown_types.hpp"
#include "markdown_scalar_functions.hpp"
#include "markdown_extraction_functions.hpp"

namespace duckdb {

static void LoadInternal(DatabaseInstance &instance) {
    // Register Markdown reader
    MarkdownReader::RegisterFunction(instance);

    // Register Markdown functions
    MarkdownFunctions::Register(instance);

    // Register Markdown extraction functions
    MarkdownExtractionFunctions::Register(instance);

    // Register Markdown types
    MarkdownTypes::Register(instance);

    // Register Markdown copy functions
    RegisterMarkdownCopyFunctions(instance);
}

void MarkdownExtension::Load(DuckDB &db) {
    LoadInternal(*db.instance);
    
    // Register Markdown files as automatically recognized by DuckDB
    auto &config = DBConfig::GetConfig(*db.instance);
    
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

DUCKDB_EXTENSION_API void markdown_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::MarkdownExtension>();
}

DUCKDB_EXTENSION_API const char *markdown_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
