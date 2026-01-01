#include "markdown_reader.hpp"
#include "markdown_copy.hpp"
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
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> result;

	// Helper lambda to handle individual file paths
	auto processPath = [&](const string &markdown_path) {
		// First: check if we're dealing with just a single file that exists
		if (fs.FileExists(markdown_path)) {
			result.push_back(markdown_path);
			return;
		}

		// Second: attempt to use the path as a glob
		auto glob_files = GetGlobFiles(context, markdown_path);
		if (glob_files.size() > 0) {
			result.insert(result.end(), glob_files.begin(), glob_files.end());
			return;
		}

		// Third: if it looks like a directory, try to glob out all of the markdown children
		if (StringUtil::EndsWith(markdown_path, "/")) {
			auto md_files = GetGlobFiles(context, fs.JoinPath(markdown_path, "*.md"));
			auto markdown_files = GetGlobFiles(context, fs.JoinPath(markdown_path, "*.markdown"));
			result.insert(result.end(), md_files.begin(), md_files.end());
			result.insert(result.end(), markdown_files.begin(), markdown_files.end());
			return;
		}

		// Fourth: check if it's a directory (without trailing slash)
		try {
			if (fs.DirectoryExists(markdown_path)) {
				auto md_files = GetGlobFiles(context, fs.JoinPath(markdown_path, "*.md"));
				auto markdown_files = GetGlobFiles(context, fs.JoinPath(markdown_path, "*.markdown"));
				result.insert(result.end(), md_files.begin(), md_files.end());
				result.insert(result.end(), markdown_files.begin(), markdown_files.end());
				return;
			}
		} catch (const NotImplementedException &) {
			// File system doesn't support directory existence checking
		}

		if (ignore_errors) {
			return;
		} else if (markdown_path.find("://") != string::npos && markdown_path.find("file://") != 0) {
			throw InvalidInputException("Remote file does not exist or is not accessible: %s", markdown_path);
		} else {
			throw InvalidInputException("File or directory does not exist: %s", markdown_path);
		}
	};

	// Handle list of files
	if (path_value.type().id() == LogicalTypeId::LIST) {
		auto &file_list = ListValue::GetChildren(path_value);
		for (auto &file_value : file_list) {
			if (file_value.type().id() == LogicalTypeId::VARCHAR) {
				processPath(file_value.ToString());
			} else {
				throw InvalidInputException("File list must contain string values");
			}
		}
	} else if (path_value.type().id() == LogicalTypeId::VARCHAR) {
		// Handle string path (file, glob pattern, or directory)
		processPath(path_value.ToString());
	} else {
		throw InvalidInputException("Path must be a string or list of strings");
	}

	// Filter for markdown files and validate existence
	vector<string> markdown_files;

	for (const auto &file : result) {
		// Check if file has markdown extension
		string extension = "";
		auto dot_pos = file.find_last_of('.');
		if (dot_pos != string::npos) {
			extension = file.substr(dot_pos + 1);
		}
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

		if (extension == "md" || extension == "markdown") {
			try {
				if (fs.FileExists(file)) {
					markdown_files.push_back(file);
				} else if (!ignore_errors) {
					throw InvalidInputException("File does not exist: %s", file);
				}
			} catch (const NotImplementedException &) {
				// File system doesn't support file existence checking, assume it exists
				markdown_files.push_back(file);
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
// Glob File Handling
//===--------------------------------------------------------------------===//

vector<string> MarkdownReader::GetGlobFiles(ClientContext &context, const string &pattern) {
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> result;
	bool supports_directory_exists;
	bool is_directory;

	// Don't bother if we can't identify a glob pattern
	try {
		if (!fs.HasGlob(pattern)) {
			return result;
		}
	} catch (const NotImplementedException &) {
		return result;
	}

	// Check this once up-front and save the FS feature
	try {
		is_directory = fs.DirectoryExists(pattern);
		supports_directory_exists = true;
	} catch (const NotImplementedException &) {
		is_directory = false;
		supports_directory_exists = false;
	}

	// Given a glob path, add any file results (ignoring directories)
	try {
		for (auto &file : fs.Glob(pattern)) {
			if (!supports_directory_exists) {
				// If we can't check for directories, just add it
				result.push_back(file.path);
			} else {
				try {
					if (!fs.DirectoryExists(file.path)) {
						result.push_back(file.path);
					}
				} catch (const NotImplementedException &) {
					// Assume it's a file if we can't check
					result.push_back(file.path);
				}
			}
		}
	} catch (const NotImplementedException &) {
		// No glob support
	}

	return result;
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
			throw InvalidInputException("File %s is too large (%llu bytes, maximum is %llu bytes)", file_path,
			                            file_size, options.maximum_file_size);
		}
	}

	string content;
	content.resize(file_size);

	fs.Read(*file_handle, reinterpret_cast<void *>(content.data()), file_size);

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
	// Strip frontmatter before parsing - cmark-gfm doesn't understand YAML frontmatter
	// and will incorrectly interpret --- as setext heading underlines
	string body = markdown_utils::StripFrontmatter(content);

	// Calculate effective max_level based on max_depth
	// max_depth is relative to min_level (depth 1 = only min_level headings)
	int32_t effective_max_level = std::min(options.max_level, options.min_level + options.max_depth - 1);

	return markdown_utils::ParseSections(body, options.min_level, effective_max_level, options.include_content,
	                                     options.content_mode, options.max_content_length);
}

//===--------------------------------------------------------------------===//
// Replacement Scan Support
//===--------------------------------------------------------------------===//

unique_ptr<TableRef> MarkdownReader::ReadMarkdownReplacement(ClientContext &context, ReplacementScanInput &input,
                                                             optional_ptr<ReplacementScanData> data) {
	auto &table_name = input.table_name;
	auto &fs = FileSystem::GetFileSystem(context);

	// Check if this looks like a markdown file or pattern
	bool is_markdown_file = StringUtil::EndsWith(StringUtil::Lower(table_name), ".md") ||
	                        StringUtil::EndsWith(StringUtil::Lower(table_name), ".markdown");

	// Check if this is a glob pattern that might contain markdown files
	bool is_glob_pattern = false;
	try {
		is_glob_pattern = fs.HasGlob(table_name);
	} catch (const NotImplementedException &) {
		// File system doesn't support glob detection
		is_glob_pattern = false;
	}

	if (is_markdown_file || is_glob_pattern) {
		// Create read_markdown function call
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ConstantExpression>(Value(table_name)));

		auto function_expr = make_uniq<FunctionExpression>("read_markdown", std::move(children));
		auto result = make_uniq<TableFunctionRef>();
		result->function = std::move(function_expr);

		// Set alias for non-glob patterns
		if (!is_glob_pattern) {
			result->alias = fs.ExtractBaseName(table_name);
		}

		return std::move(result);
	}

	return nullptr;
}

//===--------------------------------------------------------------------===//
// Copy Support
//===--------------------------------------------------------------------===//

void RegisterMarkdownCopyFunctions(ExtensionLoader &loader) {
	MarkdownCopyFunction::Register(loader);
}

} // namespace duckdb
