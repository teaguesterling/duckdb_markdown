#include "markdown_scalar_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_util.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

void MarkdownFunctions::Register(DatabaseInstance &db) {
    RegisterValidationFunction(db);
    RegisterConversionFunctions(db);
    RegisterMarkdownTypeFunctions(db);
    RegisterStatsFunctions(db);
    RegisterMetadataFunctions(db);
}

void MarkdownFunctions::RegisterValidationFunction(DatabaseInstance &db) {
    auto markdown_type = MarkdownTypes::MarkdownType();
    
    // Basic Markdown validity check for VARCHAR
    ScalarFunction md_valid_varchar("md_valid", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, 
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            const auto &input_vector = args.data[0];
            
            UnaryExecutor::ExecuteWithNulls<string_t, bool>(input_vector, result, args.size(),
                [&](string_t md_str, ValidityMask &mask, idx_t idx) {
                    if (!mask.RowIsValid(idx)) {
                        return false;
                    }
                    
                    try {
                        // Basic validation - check for basic Markdown structure
                        // More sophisticated validation could be added
                        const std::string content = md_str.GetString();
                        return !content.empty();
                    } catch (...) {
                        return false;
                    }
                });
        });
    
    // Markdown validity check for MD type (always returns true for valid MD objects)
    ScalarFunction md_valid_md("md_valid", {markdown_type}, LogicalType::BOOLEAN, 
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            const auto &input_vector = args.data[0];
            UnaryExecutor::ExecuteWithNulls<string_t, bool>(input_vector, result, args.size(),
                [&](string_t md_str, ValidityMask &mask, idx_t idx) {
                    return mask.RowIsValid(idx);
                });
        });
    
    ExtensionUtil::RegisterFunction(db, md_valid_varchar);
    ExtensionUtil::RegisterFunction(db, md_valid_md);
}

void MarkdownFunctions::RegisterConversionFunctions(DatabaseInstance &db) {
    const auto markdown_type = MarkdownTypes::MarkdownType();
    
    // md_to_html function
    ScalarFunction md_to_html_fun("md_to_html", {markdown_type}, LogicalType::VARCHAR, 
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            UnaryExecutor::Execute<string_t, string_t>(
                args.data[0], result, args.size(),
                [&](string_t md_str) -> string_t {
                    if (md_str.GetSize() == 0) {
                        return string_t();
                    }
                    
                    try {
                        const std::string html_str = markdown_utils::MarkdownToHTML(md_str.GetString());
                        return StringVector::AddString(result, html_str.c_str(), html_str.length());
                    } catch (const std::exception& e) {
                        throw InvalidInputException("Error converting Markdown to HTML: %s", e.what());
                    }
                });
        });
    
    // md_to_text function (for FTS)
    ScalarFunction md_to_text_fun("md_to_text", {markdown_type}, LogicalType::VARCHAR, 
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            UnaryExecutor::Execute<string_t, string_t>(
                args.data[0], result, args.size(),
                [&](string_t md_str) -> string_t {
                    if (md_str.GetSize() == 0) {
                        return string_t();
                    }
                    
                    try {
                        const std::string text_str = markdown_utils::MarkdownToText(md_str.GetString());
                        return StringVector::AddString(result, text_str.c_str(), text_str.length());
                    } catch (const std::exception& e) {
                        throw InvalidInputException("Error converting Markdown to text: %s", e.what());
                    }
                });
        });
    
    ExtensionUtil::RegisterFunction(db, md_to_html_fun);
    ExtensionUtil::RegisterFunction(db, md_to_text_fun);
}

void MarkdownFunctions::RegisterMarkdownTypeFunctions(DatabaseInstance &db) {
    auto markdown_type = MarkdownTypes::MarkdownType();
    
    // value_to_md function (convert any value to Markdown)
    ScalarFunction value_to_md_fun("value_to_md", {LogicalType::ANY}, markdown_type, 
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            auto& input = args.data[0];

            for (idx_t row_idx = 0; row_idx < args.size(); row_idx++) {
                try {
                    Value value = input.GetValue(row_idx);
                    
                    // Convert value to Markdown representation
                    std::string md_str;
                    if (value.IsNull()) {
                        md_str = "";
                    } else {
                        // Basic conversion - could be more sophisticated
                        md_str = value.ToString();
                    }
                    
                    result.SetValue(row_idx, Value(md_str));
                } catch (const std::exception& e) {
                    result.SetValue(row_idx, Value(""));
                } catch (...) {
                    result.SetValue(row_idx, Value(""));
                }
            }
        });
    
    ExtensionUtil::RegisterFunction(db, value_to_md_fun);
}

void MarkdownFunctions::RegisterStatsFunctions(DatabaseInstance &db) {
    auto markdown_type = MarkdownTypes::MarkdownType();
    
    // md_stats function - returns a struct with document statistics
    child_list_t<LogicalType> stats_struct_types;
    stats_struct_types.push_back(std::make_pair("word_count", LogicalType::BIGINT));
    stats_struct_types.push_back(std::make_pair("char_count", LogicalType::BIGINT));
    stats_struct_types.push_back(std::make_pair("line_count", LogicalType::BIGINT));
    stats_struct_types.push_back(std::make_pair("heading_count", LogicalType::BIGINT));
    stats_struct_types.push_back(std::make_pair("code_block_count", LogicalType::BIGINT));
    stats_struct_types.push_back(std::make_pair("link_count", LogicalType::BIGINT));
    stats_struct_types.push_back(std::make_pair("reading_time_minutes", LogicalType::DOUBLE));
    
    auto stats_struct_type = LogicalType::STRUCT(stats_struct_types);
    
    ScalarFunction md_stats_fun("md_stats", {markdown_type}, stats_struct_type,
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            auto &markdown_vector = args.data[0];
            
            for (idx_t row_idx = 0; row_idx < args.size(); row_idx++) {
                try {
                    Value md_value = markdown_vector.GetValue(row_idx);
                    
                    if (md_value.IsNull()) {
                        result.SetValue(row_idx, Value());
                        continue;
                    }
                    
                    string md_str = StringValue::Get(md_value);
                    
                    if (md_str.empty()) {
                        // Return empty stats struct
                        child_list_t<Value> struct_values;
                        struct_values.push_back(std::make_pair("word_count", Value::BIGINT(0)));
                        struct_values.push_back(std::make_pair("char_count", Value::BIGINT(0)));
                        struct_values.push_back(std::make_pair("line_count", Value::BIGINT(0)));
                        struct_values.push_back(std::make_pair("heading_count", Value::BIGINT(0)));
                        struct_values.push_back(std::make_pair("code_block_count", Value::BIGINT(0)));
                        struct_values.push_back(std::make_pair("link_count", Value::BIGINT(0)));
                        struct_values.push_back(std::make_pair("reading_time_minutes", Value::DOUBLE(0.0)));
                        
                        result.SetValue(row_idx, Value::STRUCT(struct_values));
                        continue;
                    }
                    
                    auto stats = markdown_utils::CalculateStats(md_str);
                    
                    // Create struct value
                    child_list_t<Value> struct_values;
                    struct_values.push_back(std::make_pair("word_count", Value::BIGINT(stats.word_count)));
                    struct_values.push_back(std::make_pair("char_count", Value::BIGINT(stats.char_count)));
                    struct_values.push_back(std::make_pair("line_count", Value::BIGINT(stats.line_count)));
                    struct_values.push_back(std::make_pair("heading_count", Value::BIGINT(stats.heading_count)));
                    struct_values.push_back(std::make_pair("code_block_count", Value::BIGINT(stats.code_block_count)));
                    struct_values.push_back(std::make_pair("link_count", Value::BIGINT(stats.link_count)));
                    struct_values.push_back(std::make_pair("reading_time_minutes", Value::DOUBLE(stats.reading_time_minutes)));
                    
                    result.SetValue(row_idx, Value::STRUCT(struct_values));
                    
                } catch (const std::exception& e) {
                    // Return empty stats on error
                    child_list_t<Value> struct_values;
                    struct_values.push_back(std::make_pair("word_count", Value::BIGINT(0)));
                    struct_values.push_back(std::make_pair("char_count", Value::BIGINT(0)));
                    struct_values.push_back(std::make_pair("line_count", Value::BIGINT(0)));
                    struct_values.push_back(std::make_pair("heading_count", Value::BIGINT(0)));
                    struct_values.push_back(std::make_pair("code_block_count", Value::BIGINT(0)));
                    struct_values.push_back(std::make_pair("link_count", Value::BIGINT(0)));
                    struct_values.push_back(std::make_pair("reading_time_minutes", Value::DOUBLE(0.0)));
                    
                    result.SetValue(row_idx, Value::STRUCT(struct_values));
                }
            }
        });
    
    ExtensionUtil::RegisterFunction(db, md_stats_fun);
}

void MarkdownFunctions::RegisterMetadataFunctions(DatabaseInstance &db) {
    auto markdown_type = MarkdownTypes::MarkdownType();
    
    // md_extract_metadata function - extract frontmatter as JSON
    ScalarFunction md_extract_metadata_fun("md_extract_metadata", {markdown_type}, LogicalType::JSON(),
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            UnaryExecutor::Execute<string_t, string_t>(
                args.data[0], result, args.size(),
                [&](string_t md_str) -> string_t {
                    if (md_str.GetSize() == 0) {
                        return StringVector::AddString(result, "{}", 2);
                    }
                    
                    try {
                        auto metadata = markdown_utils::ExtractMetadata(md_str.GetString());
                        
                        // Convert metadata to JSON string
                        std::string json_str = "{";
                        bool first = true;
                        
                        if (!metadata.title.empty()) {
                            json_str += "\"title\":\"" + metadata.title + "\"";
                            first = false;
                        }
                        
                        if (!metadata.description.empty()) {
                            if (!first) json_str += ",";
                            json_str += "\"description\":\"" + metadata.description + "\"";
                            first = false;
                        }
                        
                        if (!metadata.date.empty()) {
                            if (!first) json_str += ",";
                            json_str += "\"date\":\"" + metadata.date + "\"";
                        }
                        
                        json_str += "}";
                        
                        return StringVector::AddString(result, json_str.c_str(), json_str.length());
                    } catch (const std::exception& e) {
                        return StringVector::AddString(result, "{}", 2);
                    }
                });
        });
    
    ExtensionUtil::RegisterFunction(db, md_extract_metadata_fun);
}

} // namespace duckdb
