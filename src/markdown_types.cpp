#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/main/extension_util.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Markdown Type Definition
//===--------------------------------------------------------------------===//

LogicalType MarkdownTypes::MarkdownType() {
    auto markdown_type = LogicalType(LogicalTypeId::VARCHAR);
    markdown_type.SetAlias("markdown");
    markdown_type.SetAlias("md");  // Also support 'md'
    return markdown_type;
}

static bool IsMarkdownType(const LogicalType& t) {
    return t.id() == LogicalTypeId::VARCHAR && t.HasAlias() && 
           (t.GetAlias() == "markdown" || t.GetAlias() == "md");
}

//===--------------------------------------------------------------------===//
// Markdown Cast Functions
//===--------------------------------------------------------------------===//

static bool MarkdownToHTMLCast(Vector& source, Vector& result, idx_t count, CastParameters& parameters) {
    UnaryExecutor::Execute<string_t, string_t>(
        source, result, count, [&](string_t md_str) -> string_t {
            if (md_str.GetSize() == 0) {
                return string_t();
            }
            
            try {
                const std::string html_str = markdown_utils::MarkdownToHTML(md_str.GetString());
                return StringVector::AddString(result, html_str.c_str(), html_str.length());
            } catch (const std::exception& e) {
                return string_t();
            }
        });
    
    return true;
}

static bool MarkdownToTextCast(Vector& source, Vector& result, idx_t count, CastParameters& parameters) {
    UnaryExecutor::Execute<string_t, string_t>(
        source, result, count, [&](string_t md_str) -> string_t {
            if (md_str.GetSize() == 0) {
                return string_t();
            }
            
            try {
                const std::string text_str = markdown_utils::MarkdownToText(md_str.GetString());
                return StringVector::AddString(result, text_str.c_str(), text_str.length());
            } catch (const std::exception& e) {
                return md_str; // Fallback to original
            }
        });
    
    return true;
}

static bool VarcharToMarkdownCast(Vector& source, Vector& result, idx_t count, CastParameters& parameters) {
    UnaryExecutor::Execute<string_t, string_t>(
        source, result, count, [&](string_t str) -> string_t {
            // For now, just pass through - could add validation/normalization later
            return str;
        });
    
    return true;
}

static bool MarkdownToVarcharCast(Vector& source, Vector& result, idx_t count, CastParameters& parameters) {
    UnaryExecutor::Execute<string_t, string_t>(
        source, result, count, [&](string_t md_str) -> string_t {
            // Pass through - Markdown is stored as VARCHAR anyway
            return md_str;
        });
    
    return true;
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownTypes::Register(DatabaseInstance& db) {
    // Get the Markdown type
    const auto markdown_type = MarkdownType();
    
    // Register the Markdown type alias in the catalog
    ExtensionUtil::RegisterType(db, "markdown", markdown_type);
    ExtensionUtil::RegisterType(db, "md", markdown_type);
    
    // Register Markdown<->VARCHAR cast functions (isomorphic - raw markdown)
    ExtensionUtil::RegisterCastFunction(db, LogicalType(LogicalTypeId::VARCHAR), markdown_type, VarcharToMarkdownCast);
    ExtensionUtil::RegisterCastFunction(db, markdown_type, LogicalType(LogicalTypeId::VARCHAR), MarkdownToVarcharCast);
}

} // namespace duckdb
