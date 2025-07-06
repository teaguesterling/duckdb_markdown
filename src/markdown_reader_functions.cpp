#include "markdown_reader.hpp"
#include "markdown_reader_files.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind Data Structures
//===--------------------------------------------------------------------===//

struct MarkdownReadDocumentBindData : public FunctionData {
    vector<string> files;
    MarkdownReader::MarkdownReadOptions options;
    
    unique_ptr<FunctionData> Copy() const override {
        auto result = make_uniq<MarkdownReadDocumentBindData>();
        result->files = files;
        result->options = options;
        return std::move(result);
    }
    
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<MarkdownReadDocumentBindData>();
        return files == other.files;
    }
};

struct MarkdownReadSectionBindData : public FunctionData {
    vector<string> files;
    MarkdownReader::MarkdownReadOptions options;
    
    unique_ptr<FunctionData> Copy() const override {
        auto result = make_uniq<MarkdownReadSectionBindData>();
        result->files = files;
        result->options = options;
        return std::move(result);
    }
    
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<MarkdownReadSectionBindData>();
        return files == other.files;
    }
};

//===--------------------------------------------------------------------===//
// Local State for Execution
//===--------------------------------------------------------------------===//

struct MarkdownReadDocumentState : public LocalTableFunctionState {
    idx_t file_index;
    bool finished;
    
    MarkdownReadDocumentState() : file_index(0), finished(false) {}
};

struct MarkdownReadSectionState : public LocalTableFunctionState {
    idx_t file_index;
    idx_t section_index;
    vector<markdown_utils::MarkdownSection> current_sections;
    bool finished;
    
    MarkdownReadSectionState() : file_index(0), section_index(0), finished(false) {}
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
    names.emplace_back("file_path");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("content");
    return_types.emplace_back(MarkdownTypes::MarkdownType());
    
    if (result->options.extract_metadata) {
        names.emplace_back("metadata");
        return_types.emplace_back(LogicalType::JSON());
    }
    
    if (result->options.include_stats) {
        names.emplace_back("stats");
        
        // Create stats struct type
        child_list_t<LogicalType> stats_struct;
        stats_struct.push_back(std::make_pair("word_count", LogicalType::BIGINT));
        stats_struct.push_back(std::make_pair("char_count", LogicalType::BIGINT));
        stats_struct.push_back(std::make_pair("line_count", LogicalType::BIGINT));
        stats_struct.push_back(std::make_pair("heading_count", LogicalType::BIGINT));
        stats_struct.push_back(std::make_pair("code_block_count", LogicalType::BIGINT));
        stats_struct.push_back(std::make_pair("link_count", LogicalType::BIGINT));
        stats_struct.push_back(std::make_pair("reading_time_minutes", LogicalType::DOUBLE));
        
        return_types.emplace_back(LogicalType::STRUCT(stats_struct));
    }
    
    return std::move(result);
}

unique_ptr<LocalTableFunctionState> MarkdownReadDocumentInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                            GlobalTableFunctionState *global_state) {
    return make_uniq<MarkdownReadDocumentState>();
}

void MarkdownReader::MarkdownReadDocumentsFunction(ClientContext &context,
                                                  TableFunctionInput &input,
                                                  DataChunk &output) {
    auto &bind_data = input.bind_data->Cast<MarkdownReadDocumentBindData>();
    auto &state = input.local_state->Cast<MarkdownReadDocumentState>();
    
    if (state.finished || state.file_index >= bind_data.files.size()) {
        output.SetCardinality(0);
        return;
    }
    
    idx_t output_idx = 0;
    
    while (state.file_index < bind_data.files.size() && output_idx < STANDARD_VECTOR_SIZE) {
        auto &file_path = bind_data.files[state.file_index];
        
        try {
            // Read file content
            string content = ReadMarkdownFile(context, file_path, bind_data.options);
            
            // Set file path
            output.data[0].SetValue(output_idx, Value(file_path));
            
            // Set content (as MD type)
            output.data[1].SetValue(output_idx, Value(content));
            
            idx_t column_idx = 2;
            
            // Set metadata if requested
            if (bind_data.options.extract_metadata) {
                auto metadata = markdown_utils::ExtractMetadata(content);
                
                // Convert to JSON string
                string json_str = "{";
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
                
                output.data[column_idx].SetValue(output_idx, Value(json_str));
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
        
        state.file_index++;
    }
    
    if (state.file_index >= bind_data.files.size()) {
        state.finished = true;
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
    
    // Define return columns for sections
    names.emplace_back("file_path");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("section_id");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("level");
    return_types.emplace_back(LogicalType::INTEGER);
    
    names.emplace_back("title");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("content");
    return_types.emplace_back(MarkdownTypes::MarkdownType());
    
    names.emplace_back("parent_id");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("position");
    return_types.emplace_back(LogicalType::INTEGER);
    
    names.emplace_back("start_line");
    return_types.emplace_back(LogicalType::BIGINT);
    
    names.emplace_back("end_line");
    return_types.emplace_back(LogicalType::BIGINT);
    
    return std::move(result);
}

unique_ptr<LocalTableFunctionState> MarkdownReadSectionInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_state) {
    return make_uniq<MarkdownReadSectionState>();
}

void MarkdownReader::MarkdownReadSectionsFunction(ClientContext &context,
                                                 TableFunctionInput &input,
                                                 DataChunk &output) {
    auto &bind_data = input.bind_data->Cast<MarkdownReadSectionBindData>();
    auto &state = input.local_state->Cast<MarkdownReadSectionState>();
    
    if (state.finished) {
        output.SetCardinality(0);
        return;
    }
    
    idx_t output_idx = 0;
    
    while (output_idx < STANDARD_VECTOR_SIZE) {
        // If we need to load a new file
        if (state.current_sections.empty() || state.section_index >= state.current_sections.size()) {
            if (state.file_index >= bind_data.files.size()) {
                state.finished = true;
                break;
            }
            
            // Load next file
            auto &file_path = bind_data.files[state.file_index];
            
            try {
                string content = ReadMarkdownFile(context, file_path, bind_data.options);
                state.current_sections = ProcessSections(content, bind_data.options);
                state.section_index = 0;
                
                // If no sections found, move to next file
                if (state.current_sections.empty()) {
                    state.file_index++;
                    continue;
                }
                
            } catch (const std::exception &e) {
                throw InvalidInputException("Error reading Markdown file %s: %s", file_path, e.what());
            }
        }
        
        // Output current section
        const auto &section = state.current_sections[state.section_index];
        const auto &file_path = bind_data.files[state.file_index];
        
        output.data[0].SetValue(output_idx, Value(file_path));
        output.data[1].SetValue(output_idx, Value(section.id));
        output.data[2].SetValue(output_idx, Value(section.level));
        output.data[3].SetValue(output_idx, Value(section.title));
        output.data[4].SetValue(output_idx, Value(section.content));
        output.data[5].SetValue(output_idx, section.parent_id.empty() ? Value() : Value(section.parent_id));
        output.data[6].SetValue(output_idx, Value(static_cast<int32_t>(section.position)));
        output.data[7].SetValue(output_idx, Value::BIGINT(static_cast<int64_t>(section.start_line)));
        output.data[8].SetValue(output_idx, Value::BIGINT(static_cast<int64_t>(section.end_line)));
        
        output_idx++;
        state.section_index++;
        
        // If we've finished all sections in this file, move to next file
        if (state.section_index >= state.current_sections.size()) {
            state.file_index++;
            state.current_sections.clear();
            state.section_index = 0;
        }
    }
    
    output.SetCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownReader::RegisterFunction(DatabaseInstance &db) {
    // Register read_markdown function
    TableFunction read_markdown_func("read_markdown", {LogicalType::VARCHAR}, MarkdownReadDocumentsFunction, MarkdownReadDocumentsBind, MarkdownReadDocumentInit);
    
    // Add named parameters
    read_markdown_func.named_parameters["extract_metadata"] = LogicalType::BOOLEAN;
    read_markdown_func.named_parameters["include_stats"] = LogicalType::BOOLEAN;
    read_markdown_func.named_parameters["normalize_content"] = LogicalType::BOOLEAN;
    read_markdown_func.named_parameters["maximum_file_size"] = LogicalType::UBIGINT;
    read_markdown_func.named_parameters["flavor"] = LogicalType::VARCHAR;
    
    ExtensionUtil::RegisterFunction(db, read_markdown_func);
    
    // Register read_markdown_sections function
    TableFunction read_sections_func("read_markdown_sections", {LogicalType::VARCHAR}, MarkdownReadSectionsFunction, MarkdownReadSectionsBind, MarkdownReadSectionInit);
    
    // Add named parameters for sections
    read_sections_func.named_parameters["extract_metadata"] = LogicalType::BOOLEAN;
    read_sections_func.named_parameters["include_stats"] = LogicalType::BOOLEAN;
    read_sections_func.named_parameters["normalize_content"] = LogicalType::BOOLEAN;
    read_sections_func.named_parameters["maximum_file_size"] = LogicalType::UBIGINT;
    read_sections_func.named_parameters["flavor"] = LogicalType::VARCHAR;
    read_sections_func.named_parameters["include_content"] = LogicalType::BOOLEAN;
    read_sections_func.named_parameters["min_level"] = LogicalType::INTEGER;
    read_sections_func.named_parameters["max_level"] = LogicalType::INTEGER;
    read_sections_func.named_parameters["include_empty_sections"] = LogicalType::BOOLEAN;
    
    ExtensionUtil::RegisterFunction(db, read_sections_func);
}

} // namespace duckdb
