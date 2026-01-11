#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duck_block_functions.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Markdown Type Definition
//===--------------------------------------------------------------------===//

LogicalType MarkdownTypes::MarkdownType() {
	auto markdown_type = LogicalType(LogicalTypeId::VARCHAR);
	markdown_type.SetAlias("markdown");
	markdown_type.SetAlias("md"); // Also support 'md'
	return markdown_type;
}

//===--------------------------------------------------------------------===//
// Duck Block Type Definition (Unified Block/Inline)
//===--------------------------------------------------------------------===//

LogicalType MarkdownTypes::DuckBlockType() {
	// Create the STRUCT type for duck_block - unified block/inline representation
	// STRUCT(kind VARCHAR, element_type VARCHAR, content VARCHAR, level INTEGER,
	//        encoding VARCHAR, attributes MAP(VARCHAR, VARCHAR), element_order INTEGER)
	// Note: This type is defined by duck_block_utils extension; we just use the shape
	child_list_t<LogicalType> struct_children;
	struct_children.push_back(make_pair("kind", LogicalType::VARCHAR));         // 'block' or 'inline'
	struct_children.push_back(make_pair("element_type", LogicalType::VARCHAR)); // 'heading', 'bold', etc.
	struct_children.push_back(make_pair("content", LogicalType::VARCHAR));
	struct_children.push_back(make_pair("level", LogicalType::INTEGER));
	struct_children.push_back(make_pair("encoding", LogicalType::VARCHAR));
	struct_children.push_back(make_pair("attributes", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)));
	struct_children.push_back(make_pair("element_order", LogicalType::INTEGER));

	return LogicalType::STRUCT(std::move(struct_children));
}

static bool IsMarkdownType(const LogicalType &t) {
	return t.id() == LogicalTypeId::VARCHAR && t.HasAlias() && (t.GetAlias() == "markdown" || t.GetAlias() == "md");
}

//===--------------------------------------------------------------------===//
// Markdown Cast Functions
//===--------------------------------------------------------------------===//

static bool MarkdownToHTMLCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](string_t md_str) -> string_t {
		if (md_str.GetSize() == 0) {
			return string_t();
		}

		try {
			const std::string html_str = markdown_utils::MarkdownToHTML(md_str.GetString());
			return StringVector::AddString(result, html_str.c_str(), html_str.length());
		} catch (const std::exception &e) {
			return string_t();
		}
	});

	return true;
}

static bool MarkdownToTextCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](string_t md_str) -> string_t {
		if (md_str.GetSize() == 0) {
			return string_t();
		}

		try {
			const std::string text_str = markdown_utils::MarkdownToText(md_str.GetString());
			return StringVector::AddString(result, text_str.c_str(), text_str.length());
		} catch (const std::exception &e) {
			return md_str; // Fallback to original
		}
	});

	return true;
}

static bool VarcharToMarkdownCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](string_t str) -> string_t {
		// For now, just pass through - could add validation/normalization later
		return str;
	});

	return true;
}

static bool MarkdownToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](string_t md_str) -> string_t {
		// Pass through - Markdown is stored as VARCHAR anyway
		return md_str;
	});

	return true;
}

//===--------------------------------------------------------------------===//
// List-to-Markdown Cast Functions
//===--------------------------------------------------------------------===//

static bool DuckBlockListToMarkdownCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	// Cast LIST(duck_block) to markdown
	// duck_block struct: kind, element_type, content, level, encoding, attributes, element_order
	for (idx_t i = 0; i < count; i++) {
		auto list_value = source.GetValue(i);

		if (list_value.IsNull()) {
			result.SetValue(i, Value());
			continue;
		}

		string markdown = DuckBlockFunctions::RenderDuckBlocksToMarkdown(list_value);
		result.SetValue(i, Value(markdown));
	}

	return true;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownTypes::Register(ExtensionLoader &loader) {
	// Get the Markdown type
	const auto markdown_type = MarkdownType();

	// Register the Markdown type alias in the catalog
	loader.RegisterType("markdown", markdown_type);
	loader.RegisterType("md", markdown_type);

	// Register Markdown<->VARCHAR cast functions (isomorphic - raw markdown)
	loader.RegisterCastFunction(LogicalType(LogicalTypeId::VARCHAR), markdown_type, VarcharToMarkdownCast,
	                            0); // Implicit cast cost 0
	loader.RegisterCastFunction(markdown_type, LogicalType(LogicalTypeId::VARCHAR), MarkdownToVarcharCast,
	                            0); // Implicit cast cost 0

	// Note: duck_block type is owned by duck_block_utils extension
	// We use the shape but don't register the type name

	// Register LIST(duck_block shape) -> markdown cast
	const auto duck_block_type = DuckBlockType();
	const auto duck_block_list_type = LogicalType::LIST(duck_block_type);
	loader.RegisterCastFunction(duck_block_list_type, markdown_type, DuckBlockListToMarkdownCast, 1);
}

} // namespace duckdb
