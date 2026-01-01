#pragma once

#include "duckdb.hpp"
#include "duckdb/function/copy_function.hpp"

namespace duckdb {

const vector<string> markdown_extensions = {"md", "markdown"};

class MarkdownExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

// Markdown copy function
CopyFunction GetMarkdownCopyFunction();

// Register Markdown copy functions
void RegisterMarkdownCopyFunctions(ExtensionLoader &loader);

} // namespace duckdb
