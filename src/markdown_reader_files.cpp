#include "markdown_reader.hpp"
#include "duckdb_compat.hpp"
#include "markdown_copy.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include <cstdio>
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Markdown File Name Recognition
//===--------------------------------------------------------------------===//

//! The final component of a path, i.e. everything after the last separator.
static string PathFileName(const string &path) {
	auto sep_pos = path.find_last_of("/\\");
	return sep_pos == string::npos ? path : path.substr(sep_pos + 1);
}

//! Lowercased extension of a file name ("" when it has none).
static string LowerExtension(const string &file_name) {
	auto dot_pos = file_name.find_last_of('.');
	if (dot_pos == string::npos) {
		return string();
	}
	// ASCII-only: a path byte >= 0x80 is negative as a signed char, which ::tolower
	// is not defined for.
	return StringUtil::Lower(file_name.substr(dot_pos + 1));
}

static bool IsMarkdownExtension(const string &extension) {
	return extension == "md" || extension == "markdown";
}

//! Does this path name a markdown file?
//!
//! Virtual filesystems supplied by other extensions decorate the path with a
//! revision, query or fragment (`git://docs/SCHEMAS.md@HEAD`, `...md?version=2`,
//! `...md#frag`), so the raw suffix is not `.md` even though the file behind it is
//! markdown (issue #31). Two spellings of the file name are therefore accepted:
//! the literal one, and the one with VFS decoration removed. Testing the literal
//! name first means this check can only ever accept more than the plain suffix
//! test did -- a local file that genuinely contains '@', '?' or '#' in its name,
//! such as `my@file.md`, is still matched on its literal name and is never newly
//! rejected.
static bool IsMarkdownFileName(const string &path) {
	// 1. The literal file name.
	if (IsMarkdownExtension(LowerExtension(PathFileName(path)))) {
		return true;
	}

	// 2. The same name with VFS decoration stripped. A query or fragment terminates
	//    the path (RFC 3986) so it is cut from the whole string, while a '@revision'
	//    suffix is cut from the final component only -- that way the '@' of a URI
	//    authority such as `sftp://user@host/doc.md` is never mistaken for
	//    decoration. Only this recognition test sees the stripped name -- the file is
	//    always opened under the original, fully decorated path.
	auto query_pos = path.find_first_of("?#");
	auto undecorated = PathFileName(query_pos == string::npos ? path : path.substr(0, query_pos));
	auto revision_pos = undecorated.find_last_of('@');
	if (revision_pos != string::npos) {
		undecorated = undecorated.substr(0, revision_pos);
	}
	return IsMarkdownExtension(LowerExtension(undecorated));
}

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
		// Check if the file names a markdown file (tolerating VFS decoration)
		if (IsMarkdownFileName(file)) {
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
	// DuckDB 2.0 changed ReplacementScanInput: `table_name` is now only the LAST
	// dot-separated component of a QualifiedName, not the whole string. A bare
	// `FROM 'docs/readme.md'` therefore arrives as table_name == "md", and
	// `FROM 'x/doc.md@HEAD'` as "md@HEAD". Neither can be recognised as markdown, so
	// this scan declined and the binder raised "No extension found that is capable of
	// reading the file" -- which is what broke test/sql/markdown_vfs_paths.test on the
	// v2.0-cyanoptera line while every 1.5.x leg stayed green.
	//
	// GetFullPath rejoins the components and returns the ORIGINAL string on both
	// lines: on v1.5.x it concatenates catalog/schema/table, and an empty catalog and
	// schema leave just the table; on v2.0 it joins every component of the
	// QualifiedName. So this is portable rather than a 2.0-only correction.
	//
	// BY VALUE, not by reference: GetFullPath returns a temporary.
	//
	// All four uses below need the full path, not just the recognition test -- passing
	// the truncated name to read_markdown would have opened the wrong file instead of
	// declining.
	const auto table_name = ReplacementScan::GetFullPath(input);
	auto &fs = FileSystem::GetFileSystem(context);

	// ===== TEMPORARY DIAGNOSTIC (#82) -- NOT FOR MERGE =====
	// REPORTS BY THROWING, because stderr does not survive the harness.
	// duckdb/scripts/ci/run_tests.py runs batches with workers=3 and prints only a
	// dot-progress display plus failure excerpts; a child's fprintf(stderr) is
	// swallowed. A previous version of this probe printed to stderr and produced
	// ZERO lines in the 2.0 log -- which looks exactly like "never entered" and is
	// not evidence of it.
	//
	// An exception, by contrast, is surfaced verbatim as "Actual result". So the two
	// outcomes are now textually distinct and both are printed:
	//   entered     -> "MDDIAG" with the strings this function actually received
	//   not entered -> the binder's own "No extension found that is capable ..."
	throw InvalidInputException(
	    "MDDIAG entered=1 table_name='%s' catalog='%s' schema='%s' full_path='%s' is_md=%d",
	    input.table_name, input.catalog_name, input.schema_name, table_name,
	    (int)IsMarkdownFileName(table_name));
	// ===== END TEMPORARY DIAGNOSTIC =====

	// Check if this looks like a markdown file or pattern
	bool is_markdown_file = IsMarkdownFileName(table_name);

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
		children.push_back(CompatConstant(Value(table_name)));

		auto function_expr = make_uniq<FunctionExpression>("read_markdown", std::move(children));
		auto result = make_uniq<TableFunctionRef>();
		result->function = std::move(function_expr);

		// Set alias for non-glob patterns
		if (!is_glob_pattern) {
			result->alias = CompatMakeName(fs.ExtractBaseName(table_name));
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
