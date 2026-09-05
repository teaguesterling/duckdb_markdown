#include "markdown_types.hpp"
#include "duckdb_compat.hpp"
#include "markdown_utils.hpp"
#include "duck_block_functions.hpp"
#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Markdown Type Definition
//===--------------------------------------------------------------------===//

LogicalType MarkdownTypes::MarkdownType() {
	// One alias per type: the second call REPLACED the first, so this type's alias
	// has always been "md" rather than "markdown" (verified: typeof('x'::markdown)
	// returns md). Both names still resolve, because each is registered as a type
	// name separately -- the alias only decides what typeof() prints. Kept as `md`
	// to preserve that observable behaviour; changing it is a separate decision.
	return CompatWithAlias(LogicalType::VARCHAR, "md");
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
			return StringVector::AddString(result, md_str); // Fallback to original
		}
	});

	return true;
}

static bool VarcharToMarkdownCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<string_t, string_t>(
	    source, result, count, [&](string_t str) -> string_t { return StringVector::AddString(result, str); });

	return true;
}

static bool MarkdownToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	UnaryExecutor::Execute<string_t, string_t>(
	    source, result, count, [&](string_t md_str) -> string_t { return StringVector::AddString(result, md_str); });

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
// duck_block 6.4: accepting the widened shape
//===--------------------------------------------------------------------===//

// The one accepted widened shape: duck_block plus a trailing `filename VARCHAR`,
// exactly. Registration and shape supplied by duck_block_utils, who own the spec.
//
// The literal "filename" becomes Vocab FIELD_FILENAME once 6.4 is vendored here;
// markdown's drift check still reports upstream 6.3, so the constant does not
// exist locally yet.
static LogicalType DuckBlockWithFilenameType() {
	auto children = StructType::GetChildTypes(MarkdownTypes::DuckBlockType());
	children.push_back(make_pair("filename", LogicalType::VARCHAR));
	return LogicalType::STRUCT(std::move(children));
}

// Hands back DuckDB's own default cast for the pair, so the cast IS the standard
// name-based struct cast: it matches children by name and skips a source child the
// target lacks, which drops `filename`. Consumers do not widen to read it.
//
// The only thing refusing the implicit path is the child-count rule in
// cast_rules.cpp, and the binder consults registered casts before that rule --
// which is why registering it at all is what makes the shape bind.
static BoundCastInfo BindDefaultDuckBlockCast(BindCastInput &input, const LogicalType &source,
                                              const LogicalType &target) {
	return DefaultCasts::GetDefaultCastFunction(input, source, target);
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

	// Accept the 6.4 widened shape wherever a duck_block is taken. Both registrations
	// are needed and neither is reached through the other by the implicit-cost rule:
	// the LIST one covers LIST(duck_block) parameters (duck_blocks_to_md,
	// duck_blocks_to_sections), the struct one covers functions taking a bare block
	// (duck_block_to_md).
	//
	// Cost 10: positive so it never beats an exact 7-field match, small so it beats
	// any to-VARCHAR fallback. Checked before adopting it that nothing here is
	// overloaded on its block argument -- each of the three takes exactly one
	// signature -- which is the condition that makes a single cost safe.
	const auto duck_block_with_filename = DuckBlockWithFilenameType();
	loader.RegisterCastFunction(duck_block_with_filename, duck_block_type, BindDefaultDuckBlockCast, 10);
	loader.RegisterCastFunction(LogicalType::LIST(duck_block_with_filename), duck_block_list_type,
	                            BindDefaultDuckBlockCast, 10);
}

} // namespace duckdb
