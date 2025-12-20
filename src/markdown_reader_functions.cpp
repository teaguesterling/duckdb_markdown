#include "markdown_reader.hpp"
#include "markdown_types.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind Data Structures
//===--------------------------------------------------------------------===//

struct MarkdownReadDocumentBindData : public TableFunctionData {
    vector<string> files;
    MarkdownReader::MarkdownReadOptions options;
    idx_t current_file_index = 0;
};

struct MarkdownReadSectionBindData : public TableFunctionData {
    vector<string> files;
    MarkdownReader::MarkdownReadOptions options;
    vector<markdown_utils::MarkdownSection> all_sections;
    idx_t current_section_index = 0;
};


//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

static void ParseMarkdownOptions(TableFunctionBindInput &input, MarkdownReader::MarkdownReadOptions &options) {
    for (const auto &kv : input.named_parameters) {
        if (kv.first == "extract_metadata") {
            options.extract_metadata = BooleanValue::Get(kv.second);
        } else if (kv.first == "include_stats") {
            options.include_stats = BooleanValue::Get(kv.second);
        } else if (kv.first == "normalize_content") {
            options.normalize_content = BooleanValue::Get(kv.second);
        } else if (kv.first == "maximum_file_size") {
            options.maximum_file_size = UBigIntValue::Get(kv.second);
        } else if (kv.first == "flavor") {
            const auto flavor_str = StringValue::Get(kv.second);
            if (flavor_str == "gfm") {
                options.flavor = markdown_utils::MarkdownFlavor::GFM;
            } else if (flavor_str == "commonmark") {
                options.flavor = markdown_utils::MarkdownFlavor::COMMONMARK;
            } else if (flavor_str == "multimarkdown") {
                options.flavor = markdown_utils::MarkdownFlavor::MULTIMARKDOWN;
            } else {
                throw InvalidInputException("Unknown markdown flavor: %s", flavor_str);
            }
        } else if (kv.first == "include_content") {
            options.include_content = BooleanValue::Get(kv.second);
        } else if (kv.first == "min_level") {
            options.min_level = IntegerValue::Get(kv.second);
        } else if (kv.first == "max_level") {
            options.max_level = IntegerValue::Get(kv.second);
        } else if (kv.first == "include_empty_sections") {
            options.include_empty_sections = BooleanValue::Get(kv.second);
        } else if (kv.first == "include_filepath") {
            options.include_filepath = BooleanValue::Get(kv.second);
        } else if (kv.first == "content_as_varchar") {
            options.content_as_varchar = BooleanValue::Get(kv.second);
        } else {
            throw InvalidInputException("Unknown parameter for read_markdown: %s", kv.first);
        }
    }
}

//===--------------------------------------------------------------------===//
// Document Reader Implementation
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownReader::MarkdownReadDocumentsBind(ClientContext &context,
                                                                   TableFunctionBindInput &input,
                                                                   vector<LogicalType> &return_types,
                                                                   vector<string> &names) {
    auto result = make_uniq<MarkdownReadDocumentBindData>();
    
    // Parse the file path parameter
    if (input.inputs.empty()) {
        throw InvalidInputException("read_markdown requires at least one argument");
    }
    
    auto &path_param = input.inputs[0];
    result->files = GetFiles(context, path_param, false);
    
    // Parse options
    ParseMarkdownOptions(input, result->options);
    
    // Define return columns
    if (result->options.include_filepath) {
        names.emplace_back("file_path");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }
    
    names.emplace_back("content");
    if (result->options.content_as_varchar) {
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    } else {
        return_types.emplace_back(MarkdownTypes::MarkdownType());
    }
    
    if (result->options.extract_metadata) {
        names.emplace_back("metadata");
        return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
    }
    
    if (result->options.include_stats) {
        names.emplace_back("stats");
        
        // Create stats struct type
        child_list_t<LogicalType> stats_struct;
        stats_struct.push_back(make_pair("word_count", LogicalType(LogicalTypeId::BIGINT)));
        stats_struct.push_back(make_pair("char_count", LogicalType(LogicalTypeId::BIGINT)));
        stats_struct.push_back(make_pair("line_count", LogicalType(LogicalTypeId::BIGINT)));
        stats_struct.push_back(make_pair("heading_count", LogicalType(LogicalTypeId::BIGINT)));
        stats_struct.push_back(make_pair("code_block_count", LogicalType(LogicalTypeId::BIGINT)));
        stats_struct.push_back(make_pair("link_count", LogicalType(LogicalTypeId::BIGINT)));
        stats_struct.push_back(make_pair("reading_time_minutes", LogicalType(LogicalTypeId::DOUBLE)));
        
        return_types.emplace_back(LogicalType::STRUCT(stats_struct));
    }
    
    return std::move(result);
}

void MarkdownReader::MarkdownReadDocumentsFunction(ClientContext &context,
                                                  TableFunctionInput &input,
                                                  DataChunk &output) {
    auto &bind_data = input.bind_data->CastNoConst<MarkdownReadDocumentBindData>();
    
    if (bind_data.current_file_index >= bind_data.files.size()) {
        output.SetCardinality(0);
        return;
    }
    
    idx_t output_idx = 0;
    
    while (bind_data.current_file_index < bind_data.files.size() && output_idx < STANDARD_VECTOR_SIZE) {
        auto &file_path = bind_data.files[bind_data.current_file_index];
        
        try {
            // Read file content
            string content = ReadMarkdownFile(context, file_path, bind_data.options);
            
            idx_t column_idx = 0;
            
            // Set file path if requested
            if (bind_data.options.include_filepath) {
                output.data[column_idx].SetValue(output_idx, Value(file_path));
                column_idx++;
            }
            
            // Set content
            output.data[column_idx].SetValue(output_idx, Value(content));
            column_idx++;
            
            // Set metadata if requested
            if (bind_data.options.extract_metadata) {
                auto metadata = markdown_utils::ExtractMetadata(content);
                output.data[column_idx].SetValue(output_idx, markdown_utils::MetadataToMap(metadata));
                column_idx++;
            }
            
            // Set stats if requested
            if (bind_data.options.include_stats) {
                auto stats = markdown_utils::CalculateStats(content);
                
                // Create struct value
                child_list_t<Value> struct_values;
                struct_values.push_back(std::make_pair("word_count", Value::BIGINT(stats.word_count)));
                struct_values.push_back(std::make_pair("char_count", Value::BIGINT(stats.char_count)));
                struct_values.push_back(std::make_pair("line_count", Value::BIGINT(stats.line_count)));
                struct_values.push_back(std::make_pair("heading_count", Value::BIGINT(stats.heading_count)));
                struct_values.push_back(std::make_pair("code_block_count", Value::BIGINT(stats.code_block_count)));
                struct_values.push_back(std::make_pair("link_count", Value::BIGINT(stats.link_count)));
                struct_values.push_back(std::make_pair("reading_time_minutes", Value::DOUBLE(stats.reading_time_minutes)));
                
                output.data[column_idx].SetValue(output_idx, Value::STRUCT(struct_values));
                column_idx++;
            }
            
            output_idx++;
            
        } catch (const std::exception &e) {
            throw InvalidInputException("Error reading Markdown file %s: %s", file_path, e.what());
        }
        
        bind_data.current_file_index++;
    }
    
    output.SetCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Section Reader Implementation
//===--------------------------------------------------------------------===//

unique_ptr<FunctionData> MarkdownReader::MarkdownReadSectionsBind(ClientContext &context,
                                                                  TableFunctionBindInput &input,
                                                                  vector<LogicalType> &return_types,
                                                                  vector<string> &names) {
    auto result = make_uniq<MarkdownReadSectionBindData>();
    
    // Parse the file path parameter
    if (input.inputs.empty()) {
        throw InvalidInputException("read_markdown_sections requires at least one argument");
    }
    
    auto &path_param = input.inputs[0];
    result->files = GetFiles(context, path_param, false);
    
    // Parse options
    ParseMarkdownOptions(input, result->options);
    
    // Pre-process all files to extract sections
    for (const auto &file_path : result->files) {
        try {
            string content = ReadMarkdownFile(context, file_path, result->options);

            // Add frontmatter as a special section if extract_metadata is enabled
            if (result->options.extract_metadata) {
                string frontmatter = markdown_utils::ExtractRawFrontmatter(content);
                if (!frontmatter.empty()) {
                    markdown_utils::MarkdownSection fm_section;
                    fm_section.id = "frontmatter";
                    fm_section.section_path = "frontmatter";
                    fm_section.level = 0;  // Special level for frontmatter
                    fm_section.title = file_path + "|frontmatter";
                    fm_section.content = frontmatter;
                    fm_section.parent_id = "";
                    fm_section.position = 0;
                    fm_section.start_line = 1;
                    // Calculate end line from frontmatter content
                    fm_section.end_line = static_cast<idx_t>(
                        std::count(frontmatter.begin(), frontmatter.end(), '\n') + 2);  // +2 for --- delimiters
                    result->all_sections.push_back(fm_section);
                }
            }

            auto sections = ProcessSections(content, result->options);

            // Add file path to each section for later retrieval
            for (auto &section : sections) {
                section.title = file_path + "|" + section.title; // Store file path temporarily
                result->all_sections.push_back(section);
            }
        } catch (const std::exception &e) {
            // Skip files that can't be read
            continue;
        }
    }
    
    // Define return columns for sections
    if (result->options.include_filepath) {
        names.emplace_back("file_path");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }
    
    names.emplace_back("section_id");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    names.emplace_back("section_path");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

    names.emplace_back("level");
    return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
    
    names.emplace_back("title");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    
    names.emplace_back("content");
    if (result->options.content_as_varchar) {
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    } else {
        return_types.emplace_back(MarkdownTypes::MarkdownType());
    }
    
    names.emplace_back("parent_id");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    
    names.emplace_back("start_line");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    
    names.emplace_back("end_line");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    
    return std::move(result);
}

void MarkdownReader::MarkdownReadSectionsFunction(ClientContext &context,
                                                 TableFunctionInput &input,
                                                 DataChunk &output) {
    auto &bind_data = input.bind_data->CastNoConst<MarkdownReadSectionBindData>();
    
    if (bind_data.current_section_index >= bind_data.all_sections.size()) {
        output.SetCardinality(0);
        return;
    }
    
    idx_t output_idx = 0;
    
    while (bind_data.current_section_index < bind_data.all_sections.size() && output_idx < STANDARD_VECTOR_SIZE) {
        const auto &section = bind_data.all_sections[bind_data.current_section_index];
        
        // Extract file path from temporarily stored title (file_path|actual_title)
        auto pipe_pos = section.title.find('|');
        string file_path = section.title.substr(0, pipe_pos);
        string actual_title = section.title.substr(pipe_pos + 1);
        
        idx_t column_idx = 0;
        
        // Set file path if requested
        if (bind_data.options.include_filepath) {
            output.data[column_idx].SetValue(output_idx, Value(file_path));
            column_idx++;
        }
        
        output.data[column_idx].SetValue(output_idx, Value(section.id));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, Value(section.section_path));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, Value(section.level));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, Value(actual_title));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, Value(section.content));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, section.parent_id.empty() ? Value() : Value(section.parent_id));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, Value::BIGINT(static_cast<int64_t>(section.start_line)));
        column_idx++;
        output.data[column_idx].SetValue(output_idx, Value::BIGINT(static_cast<int64_t>(section.end_line)));
        
        output_idx++;
        bind_data.current_section_index++;
    }
    
    output.SetCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownReader::RegisterFunction(ExtensionLoader &loader) {
    // Register read_markdown function
    TableFunction read_markdown_func("read_markdown", {LogicalType(LogicalTypeId::VARCHAR)}, MarkdownReadDocumentsFunction, MarkdownReadDocumentsBind);

    // Add named parameters
    read_markdown_func.named_parameters["extract_metadata"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_markdown_func.named_parameters["include_stats"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_markdown_func.named_parameters["normalize_content"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_markdown_func.named_parameters["maximum_file_size"] = LogicalType(LogicalTypeId::UBIGINT);
    read_markdown_func.named_parameters["flavor"] = LogicalType(LogicalTypeId::VARCHAR);
    read_markdown_func.named_parameters["include_filepath"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_markdown_func.named_parameters["content_as_varchar"] = LogicalType(LogicalTypeId::BOOLEAN);

    loader.RegisterFunction(read_markdown_func);

    // Register read_markdown_sections function
    TableFunction read_sections_func("read_markdown_sections", {LogicalType(LogicalTypeId::VARCHAR)}, MarkdownReadSectionsFunction, MarkdownReadSectionsBind);

    // Add named parameters for sections
    read_sections_func.named_parameters["extract_metadata"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_sections_func.named_parameters["include_stats"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_sections_func.named_parameters["normalize_content"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_sections_func.named_parameters["maximum_file_size"] = LogicalType(LogicalTypeId::UBIGINT);
    read_sections_func.named_parameters["flavor"] = LogicalType(LogicalTypeId::VARCHAR);
    read_sections_func.named_parameters["include_content"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_sections_func.named_parameters["min_level"] = LogicalType(LogicalTypeId::INTEGER);
    read_sections_func.named_parameters["max_level"] = LogicalType(LogicalTypeId::INTEGER);
    read_sections_func.named_parameters["include_empty_sections"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_sections_func.named_parameters["include_filepath"] = LogicalType(LogicalTypeId::BOOLEAN);
    read_sections_func.named_parameters["content_as_varchar"] = LogicalType(LogicalTypeId::BOOLEAN);

    loader.RegisterFunction(read_sections_func);
}

} // namespace duckdb
