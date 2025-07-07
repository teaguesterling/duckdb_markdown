#include "markdown_extraction_functions.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Code Block Extraction
//===--------------------------------------------------------------------===//

struct CodeBlockExtractionBindData : public TableFunctionData {
    string language_filter;
    vector<markdown_utils::CodeBlock> code_blocks;
    idx_t current_index = 0;
};

static unique_ptr<FunctionData> CodeBlockExtractionBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
    auto result = make_uniq<CodeBlockExtractionBindData>();
    
    // Parse language filter parameter if provided
    for (const auto &kv : input.named_parameters) {
        if (kv.first == "language") {
            result->language_filter = StringValue::Get(kv.second);
        } else {
            throw InvalidInputException("Unknown parameter for md_extract_code_blocks: %s", kv.first);
        }
    }
    
    // Extract markdown content from the input parameter
    if (input.inputs.empty()) {
        throw InvalidInputException("md_extract_code_blocks requires markdown content as input");
    }
    
    auto &markdown_param = input.inputs[0];
    string markdown_content = StringValue::Get(markdown_param);
    
    // Extract code blocks using utility function (filtering handled internally)
    result->code_blocks = markdown_utils::ExtractCodeBlocks(markdown_content, result->language_filter);
    
    // Define return columns
    names.emplace_back("language");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    
    names.emplace_back("code");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    
    names.emplace_back("line_number");
    return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    
    names.emplace_back("info_string");
    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    
    return std::move(result);
}

static void CodeBlockExtractionFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &bind_data = input.bind_data->CastNoConst<CodeBlockExtractionBindData>();
    
    if (bind_data.current_index >= bind_data.code_blocks.size()) {
        output.SetCardinality(0);
        return;
    }
    
    idx_t output_idx = 0;
    
    while (bind_data.current_index < bind_data.code_blocks.size() && output_idx < STANDARD_VECTOR_SIZE) {
        const auto &block = bind_data.code_blocks[bind_data.current_index];
        
        output.data[0].SetValue(output_idx, Value(block.language));
        output.data[1].SetValue(output_idx, Value(block.code));
        output.data[2].SetValue(output_idx, Value::BIGINT(static_cast<int64_t>(block.line_number)));
        output.data[3].SetValue(output_idx, Value(block.info_string));
        
        output_idx++;
        bind_data.current_index++;
    }
    
    output.SetCardinality(output_idx);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void MarkdownExtractionFunctions::Register(DatabaseInstance &db) {
    // Register md_extract_code_blocks table function
    TableFunction code_blocks_func("md_extract_code_blocks", {MarkdownTypes::MarkdownType()}, 
                                   CodeBlockExtractionFunction, CodeBlockExtractionBind);
    code_blocks_func.named_parameters["language"] = LogicalType(LogicalTypeId::VARCHAR);
    ExtensionUtil::RegisterFunction(db, code_blocks_func);
}

} // namespace duckdb