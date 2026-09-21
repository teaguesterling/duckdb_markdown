#include "markdown_scalar_functions.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/main/client_context.hpp"
#include "markdown_types.hpp"
#include "markdown_utils.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

void MarkdownFunctions::Register(ExtensionLoader &loader) {
	RegisterValidationFunction(loader);
	RegisterConversionFunctions(loader);
	RegisterMarkdownTypeFunctions(loader);
	RegisterStatsFunctions(loader);
	RegisterMetadataFunctions(loader);
}

void MarkdownFunctions::RegisterValidationFunction(ExtensionLoader &loader) {
	// md_valid function - validates markdown content
	// Only register VARCHAR version since md type is a VARCHAR alias and will auto-cast
	ScalarFunction md_valid_fun("md_valid", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                            [](DataChunk &args, ExpressionState &state, Vector &result) {
		                            auto &input_vector = args.data[0];

		                            // Was UnaryExecutor::ExecuteWithNulls, which DuckDB v2.0
		                            // removed. A plain Execute is the exact equivalent here,
		                            // not an approximation: ExecuteWithNulls copies the input
		                            // validity into the result mask and then SKIPS the lambda
		                            // for null rows, so the old lambda's `if (!RowIsValid)
		                            // return false` branch was unreachable and a NULL input
		                            // has always produced NULL. Execute propagates nulls the
		                            // same way, so this preserves that exactly.
		                            //
		                            // Do not "simplify" this into a loop that writes false for
		                            // null inputs -- that silently turns NULL into FALSE for
		                            // non-constant inputs, which is the regression this
		                            // comment exists to prevent.
		                            UnaryExecutor::Execute<string_t, bool>(input_vector, result, args.size(),
		                                                                   [](string_t md_str) {
			                                                                   try {
				                                                                   return !md_str.GetString().empty();
			                                                                   } catch (...) {
				                                                                   return false;
			                                                                   }
		                                                                   });
	                            });

	CreateScalarFunctionInfo info(std::move(md_valid_fun));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"markdown"};
	desc.description = "Validate markdown content.";
	desc.examples = {"md_valid('# Hello')"};
	desc.categories = {"markdown"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

void MarkdownFunctions::RegisterConversionFunctions(ExtensionLoader &loader) {
	const auto markdown_type = MarkdownTypes::MarkdownType();

	// md_to_html function
	ScalarFunction md_to_html_fun(
	    "md_to_html", {markdown_type}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    UnaryExecutor::Execute<string_t, string_t>(
		        args.data[0], result, args.size(), [&](string_t md_str) -> string_t {
			        if (md_str.GetSize() == 0) {
				        return string_t();
			        }

			        try {
				        const std::string html_str = markdown_utils::MarkdownToHTML(md_str.GetString());
				        return StringVector::AddString(result, html_str.c_str(), html_str.length());
			        } catch (const std::exception &e) {
				        throw InvalidInputException("Error converting Markdown to HTML: %s", e.what());
			        }
		        });
	    });

	// md_to_text function (for FTS)
	ScalarFunction md_to_text_fun(
	    "md_to_text", {markdown_type}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    UnaryExecutor::Execute<string_t, string_t>(
		        args.data[0], result, args.size(), [&](string_t md_str) -> string_t {
			        if (md_str.GetSize() == 0) {
				        return string_t();
			        }

			        try {
				        const std::string text_str = markdown_utils::MarkdownToText(md_str.GetString());
				        return StringVector::AddString(result, text_str.c_str(), text_str.length());
			        } catch (const std::exception &e) {
				        throw InvalidInputException("Error converting Markdown to text: %s", e.what());
			        }
		        });
	    });

	// Both convert through a third-party renderer and rethrow its failures as
	// InvalidInputException. v2.0 requires a scalar function that can throw at
	// execution time to declare it, or the throw becomes an InternalException
	// complaining the function is not marked fallible.
	//
	// Called directly, NOT through a compat shim: SetFallible() is identical on
	// v1.5 (function.hpp:211) and on main, so a probe would take the same branch
	// on both -- feature detection that detects nothing.
	//
	// It is NOT inert on v1.5 either. `errors` feeds BoundFunctionExpression::
	// CanThrow(), which gates conjunct reordering (expression_heuristics,
	// adaptive_filter), filter pushdown (pushdown_get/_projection/_outer_join)
	// and dictionary-expression caching (execute_function.cpp). Declaring it
	// makes the shipped planner strictly more conservative around these two
	// functions -- safe in direction, but a real change, and measured rather
	// than assumed (see test/sql/markdown_fallible_planner.test).
	md_to_html_fun.SetFallible();
	md_to_text_fun.SetFallible();

	{
		CreateScalarFunctionInfo info(std::move(md_to_html_fun));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Convert Markdown text to HTML.";
		desc.examples = {"md_to_html('# Title')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
	{
		CreateScalarFunctionInfo info(std::move(md_to_text_fun));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		FunctionDescription desc;
		desc.parameter_names = {"markdown"};
		desc.description = "Convert Markdown text to plain text.";
		desc.examples = {"md_to_text('# Title')"};
		desc.categories = {"markdown"};
		info.descriptions.push_back(desc);
		loader.RegisterFunction(std::move(info));
	}
}

void MarkdownFunctions::RegisterMarkdownTypeFunctions(ExtensionLoader &loader) {
	auto markdown_type = MarkdownTypes::MarkdownType();

	// value_to_md function (convert any value to Markdown)
	ScalarFunction value_to_md_fun("value_to_md", {LogicalType::ANY}, markdown_type,
	                               [](DataChunk &args, ExpressionState &state, Vector &result) {
		                               auto &input = args.data[0];

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
			                               } catch (const std::exception &e) {
				                               result.SetValue(row_idx, Value(""));
			                               } catch (...) {
				                               result.SetValue(row_idx, Value(""));
			                               }
		                               }
	                               });

	CreateScalarFunctionInfo info(std::move(value_to_md_fun));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription desc;
	desc.parameter_names = {"val"};
	desc.description = "Convert any value to a Markdown formatted string.";
	desc.examples = {"value_to_md(42)"};
	desc.categories = {"markdown"};
	info.descriptions.push_back(desc);
	loader.RegisterFunction(std::move(info));
}

namespace {

// The all-zero stats struct returned for empty input and for any input that
// throws, shared by both md_stats overloads.
Value EmptyStatsValue() {
	child_list_t<Value> struct_values;
	struct_values.push_back(std::make_pair("word_count", Value::BIGINT(0)));
	struct_values.push_back(std::make_pair("char_count", Value::BIGINT(0)));
	struct_values.push_back(std::make_pair("line_count", Value::BIGINT(0)));
	struct_values.push_back(std::make_pair("heading_count", Value::BIGINT(0)));
	struct_values.push_back(std::make_pair("code_block_count", Value::BIGINT(0)));
	struct_values.push_back(std::make_pair("link_count", Value::BIGINT(0)));
	struct_values.push_back(std::make_pair("reading_time_minutes", Value::DOUBLE(0.0)));
	return Value::STRUCT(std::move(struct_values));
}

Value StatsValue(const markdown_utils::MarkdownStats &stats) {
	child_list_t<Value> struct_values;
	struct_values.push_back(std::make_pair("word_count", Value::BIGINT(stats.word_count)));
	struct_values.push_back(std::make_pair("char_count", Value::BIGINT(stats.char_count)));
	struct_values.push_back(std::make_pair("line_count", Value::BIGINT(stats.line_count)));
	struct_values.push_back(std::make_pair("heading_count", Value::BIGINT(stats.heading_count)));
	struct_values.push_back(std::make_pair("code_block_count", Value::BIGINT(stats.code_block_count)));
	struct_values.push_back(std::make_pair("link_count", Value::BIGINT(stats.link_count)));
	struct_values.push_back(std::make_pair("reading_time_minutes", Value::DOUBLE(stats.reading_time_minutes)));
	return Value::STRUCT(std::move(struct_values));
}

// `has_exact_arg` says whether this is the two-argument overload. The flag is
// read per row, so a non-constant expression works. Both overloads use DuckDB's
// default null handling, so a NULL in either argument yields a NULL struct
// without the function running at all; the IsNull guard below is belt and
// braces for the paths that do not fold.
void ExecuteStats(DataChunk &args, Vector &result, bool has_exact_arg) {
	auto &markdown_vector = args.data[0];

	for (idx_t row_idx = 0; row_idx < args.size(); row_idx++) {
		try {
			Value md_value = markdown_vector.GetValue(row_idx);

			if (md_value.IsNull()) {
				result.SetValue(row_idx, Value());
				continue;
			}

			bool exact = false;
			if (has_exact_arg) {
				Value exact_value = args.data[1].GetValue(row_idx);
				exact = !exact_value.IsNull() && BooleanValue::Get(exact_value);
			}

			string md_str = StringValue::Get(md_value);
			if (md_str.empty()) {
				result.SetValue(row_idx, EmptyStatsValue());
				continue;
			}

			result.SetValue(row_idx, StatsValue(markdown_utils::CalculateStats(md_str, exact)));
		} catch (const std::exception &e) {
			result.SetValue(row_idx, EmptyStatsValue());
		}
	}
}

} // namespace

void MarkdownFunctions::RegisterStatsFunctions(ExtensionLoader &loader) {
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

	ScalarFunction md_stats_fun(
	    "md_stats", {markdown_type}, stats_struct_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) { ExecuteStats(args, result, false); });

	ScalarFunction md_stats_exact_fun(
	    "md_stats", {markdown_type, LogicalType::BOOLEAN}, stats_struct_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) { ExecuteStats(args, result, true); });

	ScalarFunctionSet stats_set("md_stats");
	stats_set.AddFunction(md_stats_fun);
	stats_set.AddFunction(md_stats_exact_fun);

	CreateScalarFunctionInfo stats_info(std::move(stats_set));
	stats_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription stats_desc1;
	stats_desc1.parameter_names = {"markdown"};
	stats_desc1.description = "Calculate document statistics for Markdown content.";
	stats_desc1.examples = {"md_stats('# Heading\nText')"};
	stats_desc1.categories = {"markdown"};
	stats_info.descriptions.push_back(stats_desc1);

	FunctionDescription stats_desc2;
	stats_desc2.parameter_names = {"markdown", "exact"};
	stats_desc2.description = "Calculate document statistics for Markdown content with exact counts.";
	stats_desc2.examples = {"md_stats('# Heading\nText', true)"};
	stats_desc2.categories = {"markdown"};
	stats_info.descriptions.push_back(stats_desc2);
	loader.RegisterFunction(std::move(stats_info));

	// Register md_extract_section function (2-arg version: uses minimal mode)
	ScalarFunction md_extract_section(
	    "md_extract_section", {markdown_type, LogicalType::VARCHAR}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &markdown_vector = args.data[0];
		    auto &section_id_vector = args.data[1];

		    BinaryExecutor::Execute<string_t, string_t, string_t>(
		        markdown_vector, section_id_vector, result, args.size(),
		        [&](string_t markdown_str, string_t section_id_str) -> string_t {
			        if (markdown_str.GetSize() == 0 || section_id_str.GetSize() == 0) {
				        return string_t();
			        }

			        try {
				        const std::string section_content =
				            markdown_utils::ExtractSection(markdown_str.GetString(), section_id_str.GetString(), false);
				        return StringVector::AddString(result, section_content);
			        } catch (const std::exception &e) {
				        return string_t();
			        }
		        });
	    });

	// Register md_extract_section overload with include_subsections parameter
	// include_subsections=true uses 'full' mode, false uses 'minimal' mode
	ScalarFunction md_extract_section_with_subsections(
	    "md_extract_section", {markdown_type, LogicalType::VARCHAR, LogicalType::BOOLEAN}, markdown_type,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &markdown_vector = args.data[0];
		    auto &section_id_vector = args.data[1];
		    auto &include_subsections_vector = args.data[2];

		    for (idx_t i = 0; i < args.size(); i++) {
			    auto md_value = markdown_vector.GetValue(i);
			    auto section_id_value = section_id_vector.GetValue(i);
			    auto include_subsections_value = include_subsections_vector.GetValue(i);

			    if (md_value.IsNull() || section_id_value.IsNull()) {
				    result.SetValue(i, Value());
				    continue;
			    }

			    std::string markdown_str = md_value.ToString();
			    std::string section_id_str = section_id_value.ToString();
			    bool include_subsections =
			        include_subsections_value.IsNull() ? false : include_subsections_value.GetValue<bool>();

			    if (markdown_str.empty() || section_id_str.empty()) {
				    result.SetValue(i, Value(""));
				    continue;
			    }

			    try {
				    const std::string section_content =
				        markdown_utils::ExtractSection(markdown_str, section_id_str, include_subsections);
				    result.SetValue(i, Value(section_content));
			    } catch (const std::exception &e) {
				    result.SetValue(i, Value(""));
			    }
		    }
	    });

	ScalarFunctionSet extract_sec_set("md_extract_section");
	extract_sec_set.AddFunction(md_extract_section);
	extract_sec_set.AddFunction(md_extract_section_with_subsections);

	CreateScalarFunctionInfo sec_info(std::move(extract_sec_set));
	sec_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription sec_desc1;
	sec_desc1.parameter_names = {"markdown", "section_id"};
	sec_desc1.description = "Extract content of a specific section by heading title or ID.";
	sec_desc1.examples = {"md_extract_section('# Intro\nText', 'Intro')"};
	sec_desc1.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc1);

	FunctionDescription sec_desc2;
	sec_desc2.parameter_names = {"markdown", "section_id", "include_subsections"};
	sec_desc2.description =
	    "Extract content of a specific section by heading title or ID, optionally including subsections.";
	sec_desc2.examples = {"md_extract_section('# Intro\n## Sub\nText', 'Intro', true)"};
	sec_desc2.categories = {"markdown"};
	sec_info.descriptions.push_back(sec_desc2);
	loader.RegisterFunction(std::move(sec_info));

	// Register md_section_breadcrumb function
	ScalarFunction md_section_breadcrumb(
	    "md_section_breadcrumb", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &file_path_vector = args.data[0];
		    auto &section_id_vector = args.data[1];

		    BinaryExecutor::Execute<string_t, string_t, string_t>(
		        file_path_vector, section_id_vector, result, args.size(),
		        [&](string_t file_path_str, string_t section_id_str) -> string_t {
			        if (file_path_str.GetSize() == 0 || section_id_str.GetSize() == 0) {
				        return string_t();
			        }

			        const std::string breadcrumb =
			            markdown_utils::GenerateBreadcrumb(file_path_str.GetString(), section_id_str.GetString());
			        return StringVector::AddString(result, breadcrumb);
		        });
	    });

	CreateScalarFunctionInfo breadcrumb_info(std::move(md_section_breadcrumb));
	breadcrumb_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription breadcrumb_desc;
	breadcrumb_desc.parameter_names = {"file_path", "section_id"};
	breadcrumb_desc.description = "Generate a breadcrumb string combining file path and section identifier.";
	breadcrumb_desc.examples = {"md_section_breadcrumb('doc.md', 'intro')"};
	breadcrumb_desc.categories = {"markdown"};
	breadcrumb_info.descriptions.push_back(breadcrumb_desc);
	loader.RegisterFunction(std::move(breadcrumb_info));
}

void MarkdownFunctions::RegisterMetadataFunctions(ExtensionLoader &loader) {
	auto markdown_type = MarkdownTypes::MarkdownType();

	// md_extract_metadata function - extract frontmatter as MAP(VARCHAR, VARCHAR)
	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	ScalarFunction md_extract_metadata_fun(
	    "md_extract_metadata", {markdown_type}, map_type, [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &input = args.data[0];
		    auto count = args.size();

		    auto empty_map = Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, vector<Value>(), vector<Value>());

		    for (idx_t i = 0; i < count; i++) {
			    auto md_value = input.GetValue(i);
			    if (md_value.IsNull()) {
				    result.SetValue(i, empty_map);
				    continue;
			    }

			    auto md_str = md_value.ToString();
			    if (md_str.empty()) {
				    result.SetValue(i, empty_map);
				    continue;
			    }

			    try {
				    auto metadata = markdown_utils::ExtractMetadata(md_str);
				    result.SetValue(i, markdown_utils::MetadataToMap(metadata));
			    } catch (const std::exception &e) {
				    result.SetValue(i, empty_map);
			    }
		    }
	    });

	CreateScalarFunctionInfo metadata_info(std::move(md_extract_metadata_fun));
	metadata_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription metadata_desc;
	metadata_desc.parameter_names = {"markdown"};
	metadata_desc.description = "Extract YAML frontmatter from Markdown text as a MAP.";
	metadata_desc.examples = {"md_extract_metadata('---\ntitle: Test\n---\n# Body')"};
	metadata_desc.categories = {"markdown"};
	metadata_info.descriptions.push_back(metadata_desc);
	loader.RegisterFunction(std::move(metadata_info));

	// md_extract_frontmatter - the RAW frontmatter block (text between the --- fences) as VARCHAR,
	// NULL when there is no frontmatter. This is the lightweight-markdown / real-YAML seam: pair it
	// with duckdb_yaml (yaml(...) / read_yaml_frontmatter) when you need full YAML fidelity, without
	// markdown itself carrying a YAML parser.
	ScalarFunction md_extract_frontmatter_fun("md_extract_frontmatter", {markdown_type}, LogicalType::VARCHAR,
	                                          [](DataChunk &args, ExpressionState &state, Vector &result) {
		                                          auto &input = args.data[0];
		                                          auto count = args.size();

		                                          for (idx_t i = 0; i < count; i++) {
			                                          auto md_value = input.GetValue(i);
			                                          if (md_value.IsNull()) {
				                                          result.SetValue(i, Value(LogicalType::VARCHAR));
				                                          continue;
			                                          }

			                                          try {
				                                          auto raw = markdown_utils::ExtractRawFrontmatter(
				                                              md_value.ToString());
				                                          if (raw.empty()) {
					                                          // No frontmatter block (ExtractRawFrontmatter returns ""
					                                          // when absent).
					                                          result.SetValue(i, Value(LogicalType::VARCHAR));
				                                          } else {
					                                          result.SetValue(i, Value(raw));
				                                          }
			                                          } catch (const std::exception &e) {
				                                          result.SetValue(i, Value(LogicalType::VARCHAR));
			                                          }
		                                          }
	                                          });

	CreateScalarFunctionInfo frontmatter_info(std::move(md_extract_frontmatter_fun));
	frontmatter_info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	FunctionDescription frontmatter_desc;
	frontmatter_desc.parameter_names = {"markdown"};
	frontmatter_desc.description = "Extract the raw YAML frontmatter text block from Markdown.";
	frontmatter_desc.examples = {"md_extract_frontmatter('---\ntitle: Test\n---\n# Body')"};
	frontmatter_desc.categories = {"markdown"};
	frontmatter_info.descriptions.push_back(frontmatter_desc);
	loader.RegisterFunction(std::move(frontmatter_info));
}

} // namespace duckdb
