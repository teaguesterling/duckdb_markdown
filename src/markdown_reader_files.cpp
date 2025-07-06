#include "markdown_reader.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// File Path Resolution
//===--------------------------------------------------------------------===//

vector<string> MarkdownReader::GetFiles(ClientContext &context, const Value &path_value, bool ignore_errors) {
    vector<string> files;
    
    if (path_value.type().id() == LogicalTypeId::VARCHAR) {
        string path = StringValue::Get(path_value);
        
        // Handle single file or pattern
        auto &fs = FileSystem::GetFileSystem(context);
        
        if (StringUtil::Contains(path, "*") || StringUtil::Contains(path, "?")) {
            // Glob pattern
            auto glob_results = fs.Glob(path);
            for (const auto &file : glob_results) {
                files.push_back(file.path);
            }
        } else {
            // Check if it's a directory
            if (fs.DirectoryExists(path)) {
                // Add markdown files from directory
                auto dir_files = fs.Glob(fs.JoinPath(path, "*.md"));
                auto markdown_files = fs.Glob(fs.JoinPath(path, "*.markdown"));
                
                for (const auto &file : dir_files) {
                    files.push_back(file.path);
                }
                for (const auto &file : markdown_files) {
                    files.push_back(file.path);
                }
            } else {
                // Single file
                files.push_back(path);
            }
        }
    } else if (path_value.type().id() == LogicalTypeId::LIST) {
        // List of files
        auto &list_children = ListValue::GetChildren(path_value);
        for (auto &child : list_children) {
            if (child.type().id() == LogicalTypeId::VARCHAR) {
                string path = StringValue::Get(child);
                
                // Recursively resolve each path
                auto child_files = GetFiles(context, child, ignore_errors);
                files.insert(files.end(), child_files.begin(), child_files.end());
            }
        }
    } else {
        throw InvalidInputException("Path must be a string or list of strings");
    }
    
    // Filter for markdown files and validate existence
    vector<string> markdown_files;
    auto &fs = FileSystem::GetFileSystem(context);
    
    for (const auto &file : files) {
        // Check if file has markdown extension
        string extension = "";
        auto dot_pos = file.find_last_of('.');
        if (dot_pos != string::npos) {
            extension = file.substr(dot_pos + 1);
        }
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        
        if (extension == "md" || extension == "markdown") {
            if (fs.FileExists(file)) {
                markdown_files.push_back(file);
            } else if (!ignore_errors) {
                throw InvalidInputException("File does not exist: %s", file);
            }
        } else if (!ignore_errors) {
            throw InvalidInputException("File is not a markdown file: %s", file);
        }
    }
    
    // Sort files for consistent output
    std::sort(markdown_files.begin(), markdown_files.end());
    
    return markdown_files;
}

//===--------------------------------------------------------------------===//
// File Reading
//===--------------------------------------------------------------------===//

string MarkdownReader::ReadMarkdownFile(ClientContext &context, const string &file_path,
                                       const MarkdownReadOptions &options) {
    auto &fs = FileSystem::GetFileSystem(context);
    
    // Read file content
    auto file_handle = fs.OpenFile(file_path, FileOpenFlags::FILE_FLAGS_READ);
    const auto file_size = fs.GetFileSize(*file_handle);
    
    // Check file size
    if (options.maximum_file_size > 0) {
        if (file_size > options.maximum_file_size) {
            throw InvalidInputException("File %s is too large (%llu bytes, maximum is %llu bytes)", 
                                       file_path, file_size, options.maximum_file_size);
        }
    }
    
    string content;
    content.resize(file_size);
    
    fs.Read(*file_handle, reinterpret_cast<void*>(content.data()), file_size);
    
    // Normalize content if requested
    if (options.normalize_content) {
        content = markdown_utils::NormalizeMarkdown(content);
    }
    
    return content;
}

//===--------------------------------------------------------------------===//
// Section Processing
//===--------------------------------------------------------------------===//

vector<markdown_utils::MarkdownSection> MarkdownReader::ProcessSections(const string &content, 
                                                                       const MarkdownReadOptions &options) {
    return markdown_utils::ParseSections(content, 
                                       options.min_level, 
                                       options.max_level, 
                                       options.include_content);
}

//===--------------------------------------------------------------------===//
// Replacement Scan Support
//===--------------------------------------------------------------------===//

unique_ptr<TableRef> MarkdownReader::ReadMarkdownReplacement(ClientContext &context, 
                                                            ReplacementScanInput &input,
                                                            optional_ptr<ReplacementScanData> data) {
    auto &table_name = input.table_name;
    
    // Check if this looks like a markdown file
    if (StringUtil::EndsWith(StringUtil::Lower(table_name), ".md") || 
        StringUtil::EndsWith(StringUtil::Lower(table_name), ".markdown")) {
        
        // Create read_markdown function call
        vector<unique_ptr<ParsedExpression>> children;
        children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
        
        auto function_expr = make_uniq<FunctionExpression>("read_markdown", std::move(children));
        auto result = make_uniq<TableFunctionRef>();
        result->function = std::move(function_expr);
        
        return std::move(result);
    }
    
    return nullptr;
}

//===--------------------------------------------------------------------===//
// Copy Support
//===--------------------------------------------------------------------===//

void RegisterMarkdownCopyFunctions(DatabaseInstance& db) {
    // TODO: Implement COPY FROM/TO markdown support
    // This would allow:
    // COPY table FROM 'file.md' (FORMAT MARKDOWN);
    // COPY table TO 'file.md' (FORMAT MARKDOWN);
}

// CopyFunction GetMarkdownCopyFunction() {
//     // TODO: Implement markdown copy function later
// }

} // namespace duckdb
