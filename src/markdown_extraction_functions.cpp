#include "markdown_extraction_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Code Block Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void CodeBlockExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &input_vector = args.data[0];
    auto count = args.size();
    
    for (idx_t i = 0; i < count; i++) {
        auto markdown_str = input_vector.GetValue(i).ToString();
        auto code_blocks = markdown_utils::ExtractCodeBlocks(markdown_str, ""); // No language filter
        
        vector<Value> struct_values;
        for (const auto &block : code_blocks) {
            child_list_t<Value> struct_children;
            struct_children.push_back({"language", Value(block.language)});
            struct_children.push_back({"code", Value(block.code)});
            struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(block.line_number))});
            struct_children.push_back({"info_string", Value(block.info_string)});
            struct_values.push_back(Value::STRUCT(struct_children));
        }
        
        if (struct_values.empty()) {
            // For empty lists, we need to specify the type - use a simple empty list
            result.SetValue(i, Value::LIST(LogicalType::LIST(LogicalType::STRUCT({})), {}));
        } else {
            result.SetValue(i, Value::LIST(struct_values));
        }
    }
}

//===--------------------------------------------------------------------===//
// Link Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void LinkExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &input_vector = args.data[0];
    
    auto count = args.size();
    
    for (idx_t i = 0; i < count; i++) {
        auto markdown_str = input_vector.GetValue(i).ToString();
            auto links = markdown_utils::ExtractLinks(markdown_str);
            
            vector<Value> struct_values;
            for (const auto &link : links) {
                child_list_t<Value> struct_children;
                struct_children.push_back({"text", Value(link.text)});
                struct_children.push_back({"url", Value(link.url)});
                struct_children.push_back({"title", link.title.empty() ? Value() : Value(link.title)});
                struct_children.push_back({"is_reference", Value(link.is_reference)});
                struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(link.line_number))});
                struct_values.push_back(Value::STRUCT(struct_children));
            }
            
        if (struct_values.empty()) {
            // For empty lists, we need to specify the type - use a simple empty list
            result.SetValue(i, Value::LIST(LogicalType::LIST(LogicalType::STRUCT({})), {}));
        } else {
            result.SetValue(i, Value::LIST(struct_values));
        }
    }
}

//===--------------------------------------------------------------------===//
// Image Extraction - Scalar Function
//===--------------------------------------------------------------------===//

static void ImageExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &input_vector = args.data[0];
    
    auto count = args.size();
    
    for (idx_t i = 0; i < count; i++) {
        auto markdown_str = input_vector.GetValue(i).ToString();
            auto images = markdown_utils::ExtractImages(markdown_str);
            
            vector<Value> struct_values;
            for (const auto &image : images) {
                child_list_t<Value> struct_children;
                struct_children.push_back({"alt_text", Value(image.alt_text)});
                struct_children.push_back({"url", Value(image.url)});
                struct_children.push_back({"title", image.title.empty() ? Value() : Value(image.title)});
                struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(image.line_number))});
                struct_values.push_back(Value::STRUCT(struct_children));
            }
            
        if (struct_values.empty()) {
            // For empty lists, we need to specify the type - use a simple empty list
            result.SetValue(i, Value::LIST(LogicalType::LIST(LogicalType::STRUCT({})), {}));
        } else {
            result.SetValue(i, Value::LIST(struct_values));
        }
    }
}

//===--------------------------------------------------------------------===//
// Table Row Extraction - Scalar Function (renamed from md_extract_tables)
//===--------------------------------------------------------------------===//

static void TableRowExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &input_vector = args.data[0];
    
    auto count = args.size();
    
    for (idx_t i = 0; i < count; i++) {
        auto markdown_str = input_vector.GetValue(i).ToString();
            auto tables = markdown_utils::ExtractTables(markdown_str);
            
            vector<Value> struct_values;
            
            // Process each table
            for (size_t table_idx = 0; table_idx < tables.size(); table_idx++) {
                const auto &table = tables[table_idx];
                
                // Output header cells
                for (size_t col_idx = 0; col_idx < table.headers.size(); col_idx++) {
                    child_list_t<Value> struct_children;
                    struct_children.push_back({"table_index", Value::BIGINT(static_cast<int64_t>(table_idx))});
                    struct_children.push_back({"row_type", Value("header")});
                    struct_children.push_back({"row_index", Value::BIGINT(0)});
                    struct_children.push_back({"column_index", Value::BIGINT(static_cast<int64_t>(col_idx))});
                    struct_children.push_back({"cell_value", Value(table.headers[col_idx])});
                    struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(table.line_number))});
                    struct_children.push_back({"num_columns", Value::BIGINT(static_cast<int64_t>(table.num_columns))});
                    struct_children.push_back({"num_rows", Value::BIGINT(static_cast<int64_t>(table.num_rows))});
                    struct_values.push_back(Value::STRUCT(struct_children));
                }
                
                // Output data rows
                for (size_t row_idx = 0; row_idx < table.rows.size(); row_idx++) {
                    const auto &row = table.rows[row_idx];
                    for (size_t col_idx = 0; col_idx < row.size(); col_idx++) {
                        child_list_t<Value> struct_children;
                        struct_children.push_back({"table_index", Value::BIGINT(static_cast<int64_t>(table_idx))});
                        struct_children.push_back({"row_type", Value("data")});
                        struct_children.push_back({"row_index", Value::BIGINT(static_cast<int64_t>(row_idx + 1))});
                        struct_children.push_back({"column_index", Value::BIGINT(static_cast<int64_t>(col_idx))});
                        struct_children.push_back({"cell_value", Value(row[col_idx])});
                        struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(table.line_number))});
                        struct_children.push_back({"num_columns", Value::BIGINT(static_cast<int64_t>(table.num_columns))});
                        struct_children.push_back({"num_rows", Value::BIGINT(static_cast<int64_t>(table.num_rows))});
                        struct_values.push_back(Value::STRUCT(struct_children));
                    }
                }
            }
            
        if (struct_values.empty()) {
            // For empty lists, we need to specify the type - use a simple empty list
            result.SetValue(i, Value::LIST(LogicalType::LIST(LogicalType::STRUCT({})), {}));
        } else {
            result.SetValue(i, Value::LIST(struct_values));
        }
    }
}

//===--------------------------------------------------------------------===//
// Section Extraction - Scalar Function  
//===--------------------------------------------------------------------===//

static void SectionExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &input_vector = args.data[0];
    auto count = args.size();
    
    for (idx_t i = 0; i < count; i++) {
        auto markdown_str = input_vector.GetValue(i).ToString();
        auto sections = markdown_utils::ExtractSections(markdown_str, 1, 6, true); // Extract all sections with content
        
        vector<Value> struct_values;
        for (const auto &section : sections) {
            child_list_t<Value> struct_children;
            struct_children.push_back({"section_id", Value(section.id)});
            struct_children.push_back({"level", Value::INTEGER(section.level)});
            struct_children.push_back({"title", Value(section.title)});
            struct_children.push_back({"content", Value(section.content)});
            struct_children.push_back({"parent_id", section.parent_id.empty() ? Value() : Value(section.parent_id)});
            struct_children.push_back({"start_line", Value::BIGINT(static_cast<int64_t>(section.start_line))});
            struct_children.push_back({"end_line", Value::BIGINT(static_cast<int64_t>(section.end_line))});
            struct_values.push_back(Value::STRUCT(struct_children));
        }
        
        if (struct_values.empty()) {
            // For empty lists, we need to specify the type - use a simple empty list
            result.SetValue(i, Value::LIST(LogicalType::LIST(LogicalType::STRUCT({})), {}));
        } else {
            result.SetValue(i, Value::LIST(struct_values));
        }
    }
}

//===--------------------------------------------------------------------===//
// Table JSON Extraction - Scalar Function  
//===--------------------------------------------------------------------===//

static void TableJSONExtractionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &input_vector = args.data[0];
    
    auto count = args.size();
    
    for (idx_t i = 0; i < count; i++) {
        auto markdown_str = input_vector.GetValue(i).ToString();
            auto tables = markdown_utils::ExtractTables(markdown_str);
            
            vector<Value> struct_values;
            
            // Process each table
            for (size_t table_idx = 0; table_idx < tables.size(); table_idx++) {
                const auto &table = tables[table_idx];
                const auto &headers = table.headers;
                const auto &rows = table.rows;
                
                // Create header values
                vector<Value> header_values;
                for (const auto &header : headers) {
                    header_values.push_back(Value(header));
                }
                
                // Create data rows as list of lists
                vector<Value> row_values;
                for (const auto &row : rows) {
                    vector<Value> cell_values;
                    for (const auto &cell : row) {
                        cell_values.push_back(Value(cell));
                    }
                    row_values.push_back(Value::LIST(cell_values));
                }
                
                // Build JSON using DuckDB's native JSON construction
                child_list_t<Value> json_children;
                
                // Headers array
                json_children.push_back({"headers", Value::LIST(header_values)});
                
                // Data array (2D)  
                json_children.push_back({"data", Value::LIST(row_values)});
                
                // Rows as objects
                vector<Value> object_rows;
                for (const auto &row : rows) {
                    child_list_t<Value> row_obj;
                    for (size_t j = 0; j < headers.size() && j < row.size(); j++) {
                        row_obj.push_back({headers[j], Value(row[j])});
                    }
                    object_rows.push_back(Value::STRUCT(row_obj));
                }
                json_children.push_back({"rows", Value::LIST(object_rows)});
                
                // Metadata
                child_list_t<Value> metadata_children;
                metadata_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(table.line_number))});
                metadata_children.push_back({"num_columns", Value::BIGINT(static_cast<int64_t>(table.num_columns))});
                metadata_children.push_back({"num_rows", Value::BIGINT(static_cast<int64_t>(table.num_rows))});
                json_children.push_back({"metadata", Value::STRUCT(metadata_children)});
                
                Value json_value = Value::STRUCT(json_children);
                
                // Build structure description 
                child_list_t<Value> structure_children;
                structure_children.push_back({"table_name", Value("table_" + std::to_string(table_idx))});
                
                vector<Value> column_info;
                for (size_t i = 0; i < headers.size(); i++) {
                    child_list_t<Value> col_children;
                    col_children.push_back({"name", Value(headers[i])});
                    col_children.push_back({"index", Value::BIGINT(static_cast<int64_t>(i))});
                    col_children.push_back({"type", Value("string")});
                    column_info.push_back(Value::STRUCT(col_children));
                }
                structure_children.push_back({"columns", Value::LIST(column_info)});
                structure_children.push_back({"row_count", Value::BIGINT(static_cast<int64_t>(rows.size()))});
                structure_children.push_back({"source_line", Value::BIGINT(static_cast<int64_t>(table.line_number))});
                
                Value structure_value = Value::STRUCT(structure_children);
                
                // Create struct for this table
                child_list_t<Value> table_struct_children;
                table_struct_children.push_back({"table_index", Value::BIGINT(static_cast<int64_t>(table_idx))});
                table_struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(table.line_number))});
                table_struct_children.push_back({"num_columns", Value::BIGINT(static_cast<int64_t>(headers.size()))});
                table_struct_children.push_back({"num_rows", Value::BIGINT(static_cast<int64_t>(rows.size()))});
                table_struct_children.push_back({"headers", Value::LIST(header_values)});
                table_struct_children.push_back({"table_data", Value::LIST(row_values)});
                table_struct_children.push_back({"table_json", json_value});
                table_struct_children.push_back({"json_structure", structure_value});
                
                struct_values.push_back(Value::STRUCT(table_struct_children));
            }
            
        if (struct_values.empty()) {
            // For empty lists, we need to specify the type - use a simple empty list
            result.SetValue(i, Value::LIST(LogicalType::LIST(LogicalType::STRUCT({})), {}));
        } else {
            result.SetValue(i, Value::LIST(struct_values));
        }
    }
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownExtractionFunctions::Register(DatabaseInstance &db) {
    // Define return types for scalar functions
    auto code_block_struct_type = LogicalType::STRUCT({
        {"language", LogicalType(LogicalTypeId::VARCHAR)},
        {"code", LogicalType(LogicalTypeId::VARCHAR)},
        {"line_number", LogicalType(LogicalTypeId::BIGINT)},
        {"info_string", LogicalType(LogicalTypeId::VARCHAR)}
    });
    
    auto link_struct_type = LogicalType::STRUCT({
        {"text", LogicalType(LogicalTypeId::VARCHAR)},
        {"url", LogicalType(LogicalTypeId::VARCHAR)},
        {"title", LogicalType(LogicalTypeId::VARCHAR)},
        {"is_reference", LogicalType(LogicalTypeId::BOOLEAN)},
        {"line_number", LogicalType(LogicalTypeId::BIGINT)}
    });
    
    auto image_struct_type = LogicalType::STRUCT({
        {"alt_text", LogicalType(LogicalTypeId::VARCHAR)},
        {"url", LogicalType(LogicalTypeId::VARCHAR)},
        {"title", LogicalType(LogicalTypeId::VARCHAR)},
        {"line_number", LogicalType(LogicalTypeId::BIGINT)}
    });
    
    auto table_row_struct_type = LogicalType::STRUCT({
        {"table_index", LogicalType(LogicalTypeId::BIGINT)},
        {"row_type", LogicalType(LogicalTypeId::VARCHAR)},
        {"row_index", LogicalType(LogicalTypeId::BIGINT)},
        {"column_index", LogicalType(LogicalTypeId::BIGINT)},
        {"cell_value", LogicalType(LogicalTypeId::VARCHAR)},
        {"line_number", LogicalType(LogicalTypeId::BIGINT)},
        {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
        {"num_rows", LogicalType(LogicalTypeId::BIGINT)}
    });
    
    auto table_json_struct_type = LogicalType::STRUCT({
        {"table_index", LogicalType(LogicalTypeId::BIGINT)},
        {"line_number", LogicalType(LogicalTypeId::BIGINT)},
        {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
        {"num_rows", LogicalType(LogicalTypeId::BIGINT)},
        {"headers", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
        {"table_data", LogicalType::LIST(LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)))},
        {"table_json", LogicalType::STRUCT({})}, // Complex nested struct
        {"json_structure", LogicalType::STRUCT({})} // Complex nested struct
    });
    
    // Register md_extract_code_blocks scalar function  
    ScalarFunction code_blocks_func("md_extract_code_blocks", {MarkdownTypes::MarkdownType()}, 
                                    LogicalType::LIST(code_block_struct_type), CodeBlockExtractionFunction);
    ExtensionUtil::RegisterFunction(db, code_blocks_func);
    
    // Register md_extract_links scalar function
    ScalarFunction links_func("md_extract_links", {MarkdownTypes::MarkdownType()}, 
                              LogicalType::LIST(link_struct_type), LinkExtractionFunction);
    ExtensionUtil::RegisterFunction(db, links_func);
    
    // Register md_extract_images scalar function
    ScalarFunction images_func("md_extract_images", {MarkdownTypes::MarkdownType()}, 
                               LogicalType::LIST(image_struct_type), ImageExtractionFunction);
    ExtensionUtil::RegisterFunction(db, images_func);
    
    // Register md_extract_table_rows scalar function (renamed from md_extract_tables)
    ScalarFunction table_rows_func("md_extract_table_rows", {MarkdownTypes::MarkdownType()}, 
                                   LogicalType::LIST(table_row_struct_type), TableRowExtractionFunction);
    ExtensionUtil::RegisterFunction(db, table_rows_func);
    
    // Register md_extract_tables_json scalar function
    ScalarFunction tables_json_func("md_extract_tables_json", {MarkdownTypes::MarkdownType()}, 
                                    LogicalType::LIST(table_json_struct_type), TableJSONExtractionFunction);
    ExtensionUtil::RegisterFunction(db, tables_json_func);
    
    // Register md_extract_sections scalar function
    LogicalType section_struct_type = LogicalType::STRUCT({
        {"section_id", LogicalType::VARCHAR},
        {"level", LogicalType::INTEGER}, 
        {"title", LogicalType::VARCHAR},
        {"content", MarkdownTypes::MarkdownType()},
        {"parent_id", LogicalType::VARCHAR},
        {"start_line", LogicalType::BIGINT},
        {"end_line", LogicalType::BIGINT}
    });
    
    ScalarFunction sections_func("md_extract_sections", {MarkdownTypes::MarkdownType()}, 
                                LogicalType::LIST(section_struct_type), SectionExtractionFunction);
    ExtensionUtil::RegisterFunction(db, sections_func);
}

} // namespace duckdb