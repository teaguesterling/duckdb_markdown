#include "markdown_extraction_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
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
			struct_children.push_back({"title", link.title.empty() ? Value(LogicalType::VARCHAR) : Value(link.title)});
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
			struct_children.push_back(
			    {"title", image.title.empty() ? Value(LogicalType::VARCHAR) : Value(image.title)});
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
			struct_children.push_back({"section_path", Value(section.section_path)});
			struct_children.push_back({"level", Value::INTEGER(section.level)});
			struct_children.push_back({"title", Value(section.title)});
			struct_children.push_back({"content", Value(section.content)});
			struct_children.push_back(
			    {"parent_id", section.parent_id.empty() ? Value(LogicalType::VARCHAR) : Value(section.parent_id)});
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

static void SectionExtractionFunctionWithLevels(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input_vector = args.data[0];
	auto &min_level_vector = args.data[1];
	auto &max_level_vector = args.data[2];
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto markdown_str = input_vector.GetValue(i).ToString();
		auto min_level = static_cast<int32_t>(min_level_vector.GetValue(i).GetValue<int32_t>());
		auto max_level = static_cast<int32_t>(max_level_vector.GetValue(i).GetValue<int32_t>());
		auto sections = markdown_utils::ExtractSections(markdown_str, min_level, max_level, true);

		vector<Value> struct_values;
		for (const auto &section : sections) {
			child_list_t<Value> struct_children;
			struct_children.push_back({"section_id", Value(section.id)});
			struct_children.push_back({"section_path", Value(section.section_path)});
			struct_children.push_back({"level", Value::INTEGER(section.level)});
			struct_children.push_back({"title", Value(section.title)});
			struct_children.push_back({"content", Value(section.content)});
			struct_children.push_back(
			    {"parent_id", section.parent_id.empty() ? Value(LogicalType::VARCHAR) : Value(section.parent_id)});
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

// Version with content_mode parameter
static void SectionExtractionFunctionWithContentMode(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &input_vector = args.data[0];
	auto &min_level_vector = args.data[1];
	auto &max_level_vector = args.data[2];
	auto &content_mode_vector = args.data[3];
	auto count = args.size();

	for (idx_t i = 0; i < count; i++) {
		auto markdown_str = input_vector.GetValue(i).ToString();
		auto min_level = static_cast<int32_t>(min_level_vector.GetValue(i).GetValue<int32_t>());
		auto max_level = static_cast<int32_t>(max_level_vector.GetValue(i).GetValue<int32_t>());

		std::string content_mode = "minimal";
		auto mode_value = content_mode_vector.GetValue(i);
		if (!mode_value.IsNull()) {
			content_mode = mode_value.ToString();
		}

		auto sections = markdown_utils::ExtractSections(markdown_str, min_level, max_level, true, content_mode);

		vector<Value> struct_values;
		for (const auto &section : sections) {
			child_list_t<Value> struct_children;
			struct_children.push_back({"section_id", Value(section.id)});
			struct_children.push_back({"section_path", Value(section.section_path)});
			struct_children.push_back({"level", Value::INTEGER(section.level)});
			struct_children.push_back({"title", Value(section.title)});
			struct_children.push_back({"content", Value(section.content)});
			struct_children.push_back(
			    {"parent_id", section.parent_id.empty() ? Value(LogicalType::VARCHAR) : Value(section.parent_id)});
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

			// Create struct for this table
			child_list_t<Value> table_struct_children;
			table_struct_children.push_back({"table_index", Value::BIGINT(static_cast<int64_t>(table_idx))});
			table_struct_children.push_back({"line_number", Value::BIGINT(static_cast<int64_t>(table.line_number))});
			table_struct_children.push_back({"num_columns", Value::BIGINT(static_cast<int64_t>(headers.size()))});
			table_struct_children.push_back({"num_rows", Value::BIGINT(static_cast<int64_t>(rows.size()))});
			table_struct_children.push_back({"headers", Value::LIST(header_values)});
			table_struct_children.push_back({"table_data", Value::LIST(row_values)});

			struct_values.push_back(Value::STRUCT(table_struct_children));
		}

		if (struct_values.empty()) {
			// For empty lists, use the correct struct type
			auto empty_list_type = LogicalType::LIST(LogicalType::STRUCT(
			    {{"table_index", LogicalType(LogicalTypeId::BIGINT)},
			     {"line_number", LogicalType(LogicalTypeId::BIGINT)},
			     {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
			     {"num_rows", LogicalType(LogicalTypeId::BIGINT)},
			     {"headers", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
			     {"table_data", LogicalType::LIST(LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)))}}));
			result.SetValue(i, Value::LIST(empty_list_type, {}));
		} else {
			result.SetValue(i, Value::LIST(struct_values));
		}
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownExtractionFunctions::Register(ExtensionLoader &loader) {
	// Define return types for scalar functions
	auto code_block_struct_type = LogicalType::STRUCT({{"language", LogicalType(LogicalTypeId::VARCHAR)},
	                                                   {"code", LogicalType(LogicalTypeId::VARCHAR)},
	                                                   {"line_number", LogicalType(LogicalTypeId::BIGINT)},
	                                                   {"info_string", LogicalType(LogicalTypeId::VARCHAR)}});

	auto link_struct_type = LogicalType::STRUCT({{"text", LogicalType(LogicalTypeId::VARCHAR)},
	                                             {"url", LogicalType(LogicalTypeId::VARCHAR)},
	                                             {"title", LogicalType(LogicalTypeId::VARCHAR)},
	                                             {"is_reference", LogicalType(LogicalTypeId::BOOLEAN)},
	                                             {"line_number", LogicalType(LogicalTypeId::BIGINT)}});

	auto image_struct_type = LogicalType::STRUCT({{"alt_text", LogicalType(LogicalTypeId::VARCHAR)},
	                                              {"url", LogicalType(LogicalTypeId::VARCHAR)},
	                                              {"title", LogicalType(LogicalTypeId::VARCHAR)},
	                                              {"line_number", LogicalType(LogicalTypeId::BIGINT)}});

	auto table_row_struct_type = LogicalType::STRUCT({{"table_index", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"row_type", LogicalType(LogicalTypeId::VARCHAR)},
	                                                  {"row_index", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"column_index", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"cell_value", LogicalType(LogicalTypeId::VARCHAR)},
	                                                  {"line_number", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
	                                                  {"num_rows", LogicalType(LogicalTypeId::BIGINT)}});

	auto table_json_struct_type = LogicalType::STRUCT(
	    {{"table_index", LogicalType(LogicalTypeId::BIGINT)},
	     {"line_number", LogicalType(LogicalTypeId::BIGINT)},
	     {"num_columns", LogicalType(LogicalTypeId::BIGINT)},
	     {"num_rows", LogicalType(LogicalTypeId::BIGINT)},
	     {"headers", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	     {"table_data", LogicalType::LIST(LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)))}});

	// Register md_extract_code_blocks scalar function
	ScalarFunction code_blocks_func("md_extract_code_blocks", {MarkdownTypes::MarkdownType()},
	                                LogicalType::LIST(code_block_struct_type), CodeBlockExtractionFunction);
	loader.RegisterFunction(code_blocks_func);

	// Register md_extract_links scalar function
	ScalarFunction links_func("md_extract_links", {MarkdownTypes::MarkdownType()}, LogicalType::LIST(link_struct_type),
	                          LinkExtractionFunction);
	loader.RegisterFunction(links_func);

	// Register md_extract_images scalar function
	ScalarFunction images_func("md_extract_images", {MarkdownTypes::MarkdownType()},
	                           LogicalType::LIST(image_struct_type), ImageExtractionFunction);
	loader.RegisterFunction(images_func);

	// Register md_extract_table_rows scalar function (renamed from md_extract_tables)
	ScalarFunction table_rows_func("md_extract_table_rows", {MarkdownTypes::MarkdownType()},
	                               LogicalType::LIST(table_row_struct_type), TableRowExtractionFunction);
	loader.RegisterFunction(table_rows_func);

	// Register md_extract_tables_json scalar function
	ScalarFunction tables_json_func("md_extract_tables_json", {MarkdownTypes::MarkdownType()},
	                                LogicalType::LIST(table_json_struct_type), TableJSONExtractionFunction);
	loader.RegisterFunction(tables_json_func);

	// Register md_extract_sections scalar function
	LogicalType section_struct_type = LogicalType::STRUCT({{"section_id", LogicalType::VARCHAR},
	                                                       {"section_path", LogicalType::VARCHAR},
	                                                       {"level", LogicalType::INTEGER},
	                                                       {"title", LogicalType::VARCHAR},
	                                                       {"content", MarkdownTypes::MarkdownType()},
	                                                       {"parent_id", LogicalType::VARCHAR},
	                                                       {"start_line", LogicalType::BIGINT},
	                                                       {"end_line", LogicalType::BIGINT}});

	// Register main function with MARKDOWN type
	ScalarFunction sections_func("md_extract_sections", {MarkdownTypes::MarkdownType()},
	                             LogicalType::LIST(section_struct_type), SectionExtractionFunction);
	loader.RegisterFunction(sections_func);

	// Register overload for VARCHAR input
	ScalarFunction sections_varchar_func("md_extract_sections", {LogicalType::VARCHAR},
	                                     LogicalType::LIST(section_struct_type), SectionExtractionFunction);
	loader.RegisterFunction(sections_varchar_func);

	// Register overload for VARCHAR with level filtering
	ScalarFunction sections_levels_func("md_extract_sections",
	                                    {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER},
	                                    LogicalType::LIST(section_struct_type), SectionExtractionFunctionWithLevels);
	loader.RegisterFunction(sections_levels_func);

	// Register overload for VARCHAR with level filtering and content_mode
	ScalarFunction sections_content_mode_func(
	    "md_extract_sections", {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR},
	    LogicalType::LIST(section_struct_type), SectionExtractionFunctionWithContentMode);
	loader.RegisterFunction(sections_content_mode_func);
}

} // namespace duckdb
