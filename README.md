# DuckDB Markdown Extension

This extension adds Markdown processing capabilities to DuckDB, enabling structured analysis of Markdown documents and content extraction for documentation analysis, content auditing, and knowledge base processing.

## Features

- **Markdown Content Extraction**: Extract code blocks, links, images, and tables from Markdown text
- **Documentation Analysis**: Analyze large documentation repositories with SQL queries
- **Cross-Platform Support**: Works on Linux, macOS, and WebAssembly (Windows support in development)
- **GitHub Flavored Markdown**: Uses cmark-gfm for accurate parsing of modern Markdown
- **High Performance**: Process thousands of documents efficiently with robust glob pattern support

## Installation

### Loading the Extension

```sql
-- Install from community extensions (when available)
INSTALL markdown FROM community;
LOAD markdown;
```

### Building from Source

```bash
git clone https://github.com/teaguesterling/duckdb_markdown
cd duckdb_markdown
make
make test
```

## Quick Start

```sql
-- Load the extension
LOAD markdown;

-- Read Markdown files with glob patterns
SELECT content FROM read_markdown('docs/**/*.md');

-- Read documentation sections with hierarchy
SELECT title, level, content 
FROM read_markdown_sections('README.md', include_content := true);

-- Extract code blocks from Markdown text
SELECT cb.language, cb.code 
FROM (
  SELECT UNNEST(md_extract_code_blocks('```python\nprint("Hello, World!")\n```')) as cb
);

-- Analyze documentation repositories
SELECT 
  len(md_extract_code_blocks(content)) as code_examples,
  len(md_extract_links(content)) as external_links,
  len(md_extract_images(content)) as images
FROM read_markdown('**/*.md');

-- Use replacement scan syntax for convenience
SELECT * FROM '*.md';
SELECT * FROM 'docs/**/*.md';
```

## Table-like Syntax Support

The extension supports DuckDB's replacement scan feature, allowing you to query Markdown files using table-like syntax:

```sql
-- Query markdown files directly
SELECT * FROM 'README.md';
SELECT * FROM '*.md';  
SELECT * FROM 'docs/**/*.md';

-- Equivalent to calling read_markdown()
SELECT * FROM read_markdown('README.md');
SELECT * FROM read_markdown('*.md');
SELECT * FROM read_markdown('docs/**/*.md');
```

**Supported patterns in replacement scan:**
- `'file.md'` - Individual markdown files
- `'*.md'`, `'**/*.markdown'` - Glob patterns
- Recursive patterns like `'docs/**/*.md'`

**Note**: Directory patterns like `'docs/'` are not supported. Use recursive globs like `'docs/**/*.md'` instead.

## Core Functions

### File Reading Functions

Read Markdown files directly with comprehensive parameter support:

#### `read_markdown(files, [parameters...])`
Reads Markdown files and returns one row per file.

**Parameters:**
- `files` (required) - File path, glob pattern, directory, or list of mixed patterns
- `include_filepath := false` - Include file_path column in output
- `content_as_varchar := false` - Return content as VARCHAR instead of MARKDOWN type
- `maximum_file_size := 16777216` - Maximum file size in bytes (16MB default)
- `extract_metadata := true` - Extract frontmatter metadata
- `normalize_content := true` - Normalize Markdown content

**Returns:** `(content MARKDOWN, metadata MAP(VARCHAR, VARCHAR))` or `(file_path VARCHAR, content MARKDOWN, metadata MAP(VARCHAR, VARCHAR))` with `include_filepath := true`

#### `read_markdown_sections(files, [parameters...])`
Reads Markdown files and parses them into hierarchical sections.

**Parameters:**
- `files` (required) - File path, glob pattern, directory, or list of mixed patterns
- `include_content := true` - Include section content in output
- `min_level := 1` - Minimum heading level to include (1-6)
- `max_level := 6` - Maximum heading level to include (1-6)
- `include_empty_sections := false` - Include sections without content
- `include_filepath := false` - Include file_path column in output
- `extract_metadata := true` - Include frontmatter as a special section (level=0)
- Plus all `read_markdown` parameters

**Returns:** `(section_id VARCHAR, level INTEGER, title VARCHAR, content MARKDOWN, parent_id VARCHAR, start_line BIGINT, end_line BIGINT)` or `(file_path VARCHAR, section_id VARCHAR, level INTEGER, title VARCHAR, content MARKDOWN, parent_id VARCHAR, start_line BIGINT, end_line BIGINT)` with `include_filepath := true`

**Note:** When `extract_metadata := true`, YAML frontmatter is included as a special section with `level=0`, `section_id='frontmatter'`, and the raw YAML content (without `---` delimiters) as the content.

### Content Extraction Functions

All extraction functions return `LIST<STRUCT>` types for easy SQL composition:

- **`md_extract_code_blocks(markdown)`** - Extract code blocks with language and metadata
- **`md_extract_links(markdown)`** - Extract links with text, URL, and title information  
- **`md_extract_images(markdown)`** - Extract images with alt text and metadata
- **`md_extract_table_rows(markdown)`** - Extract table data as individual cells
- **`md_extract_tables_json(markdown)`** - Extract tables as structured JSON with enhanced metadata

### Document Processing Functions

- **`md_to_html(markdown)`** - Convert markdown content to HTML
- **`md_to_text(markdown)`** - Convert markdown to plain text (useful for full-text search)
- **`md_valid(markdown)`** - Validate markdown content and return boolean
- **`md_stats(markdown)`** - Get document statistics (word count, reading time, etc.)
- **`md_extract_metadata(markdown)`** - Extract frontmatter metadata as `MAP(VARCHAR, VARCHAR)`
- **`md_extract_section(markdown, section_id)`** - Extract specific section by ID
- **`md_section_breadcrumb(file_path, section_id)`** - Generate breadcrumb navigation for section
- **`value_to_md(value)`** - Convert any value to markdown representation

### Document Processing Examples

```sql
-- Convert markdown to HTML for web display
SELECT md_to_html(content) as html_content
FROM read_markdown('README.md');

-- Get document statistics
SELECT 
  filename,
  md_stats(content).word_count as words,
  md_stats(content).reading_time_minutes as reading_time
FROM read_markdown('docs/**/*.md');

-- Extract and access frontmatter metadata fields
SELECT
  filename,
  md_extract_metadata(content)['title'] as title,
  md_extract_metadata(content)['author'] as author,
  md_extract_metadata(content) as all_metadata
FROM read_markdown('docs/**/*.md')
WHERE cardinality(md_extract_metadata(content)) > 0;

-- Validate markdown content  
SELECT filename, md_valid(content::varchar) as is_valid
FROM read_markdown('**/*.md')
WHERE NOT md_valid(content::varchar);
```

## Use Cases

### Documentation Analysis

Analyze code documentation across entire repositories:

```sql
-- Find all Python examples in documentation
SELECT filename, unnest.code, unnest.line_number
FROM read_markdown('docs/**/*.md') docs,
     UNNEST(md_extract_code_blocks(docs.content))
WHERE unnest.language = 'python';

-- Audit external links in documentation  
SELECT unnest.url, count(*) as usage_count
FROM read_markdown('**/*.md') docs,
     UNNEST(md_extract_links(docs.content))
WHERE unnest.url LIKE 'http%'
GROUP BY unnest.url
ORDER BY usage_count DESC;
```

### Content Quality Assessment

Evaluate documentation completeness and quality:

```sql
-- Calculate content richness scores
SELECT 
  filename,
  len(md_extract_code_blocks(content)) * 3 +
  len(md_extract_links(content)) * 1 +  
  len(md_extract_images(content)) * 2 as richness_score
FROM read_markdown('docs/**/*.md')
ORDER BY richness_score DESC;
```

### Large-Scale Documentation Search

Create searchable knowledge bases from documentation:

```sql
-- Create searchable documentation index
CREATE TABLE docs AS
SELECT 
  title, level, content,
  row_number() OVER (ORDER BY title) as section_id
FROM read_markdown_sections('**/*.md', include_content := true);

-- Create full-text search index using plain text conversion
PRAGMA create_fts_index('docs', 'section_id', 'md_to_text(content)');

-- Search documentation
SELECT title, substring(md_to_text(content), 1, 200) as preview
FROM docs 
WHERE md_to_text(content) ILIKE '%memory optimization%'
ORDER BY title;
```

## Glob Pattern Support

The extension includes comprehensive glob pattern support across different file systems:

```sql
-- Basic patterns
SELECT * FROM read_markdown('docs/*.md');
SELECT * FROM read_markdown('**/*.markdown');

-- Recursive directory scanning
SELECT * FROM read_markdown('documentation/**/*.md');

-- Multiple patterns
SELECT * FROM read_markdown(['README.md', 'docs/**/*.md', 'examples/**/*.md']);

-- Remote file systems (S3, etc.)
SELECT * FROM read_markdown('s3://bucket/docs/*.md');
```

**Supported patterns:**
- `*.md`, `**/*.markdown` - Standard glob patterns
- `docs/**/*.md` - Recursive directory scanning
- Mixed lists combining files and glob patterns
- Remote file systems with graceful degradation

## Return Types

All functions return structured data using `LIST<STRUCT>` types:

```sql
-- Code blocks
LIST<STRUCT(language VARCHAR, code VARCHAR, line_number BIGINT, info_string VARCHAR)>

-- Links  
LIST<STRUCT(text VARCHAR, url VARCHAR, title VARCHAR, is_reference BOOLEAN, line_number BIGINT)>

-- Images
LIST<STRUCT(alt_text VARCHAR, url VARCHAR, title VARCHAR, line_number BIGINT)>

-- Table rows
LIST<STRUCT(table_index BIGINT, row_type VARCHAR, row_index BIGINT, column_index BIGINT, cell_value VARCHAR, line_number BIGINT, num_columns BIGINT, num_rows BIGINT)>
```

Use with `UNNEST()` to flatten into rows or `len()` to count elements.

## Type System and Automatic Casting

The extension defines a `MARKDOWN` type (alias: `md`) that automatically casts to/from `VARCHAR`:

```sql
-- Explicit casting
SELECT '# Hello World'::markdown;
SELECT '# Hello World'::md;

-- Automatic casting - these all work seamlessly
SELECT md_to_html('# Hello World');  -- VARCHAR automatically cast to MARKDOWN
SELECT md_stats('# Test\nContent');  -- VARCHAR automatically cast to MARKDOWN
SELECT content::varchar FROM read_markdown('README.md');  -- MARKDOWN to VARCHAR

-- Type checking
SELECT typeof('# Hello World'::markdown);  -- Returns 'md'
```

All markdown functions accept both `VARCHAR` and `MARKDOWN` types through automatic casting, making the API flexible and easy to use.

## Performance

The extension is designed for high-performance document processing:

- **4,000+ sections/second** processing rate on typical hardware
- **Memory efficient** streaming processing  
- **Parallel safe** for concurrent query execution
- **Cross-platform** robust glob support including remote file systems

**Real-world benchmark**: Processing 287 Markdown files (2,699 sections, 1,137 code blocks, 1,174 links) in 603ms.

## Current Status

**✅ Available (v1.0.1):**
- Complete file reading functions (`read_markdown`, `read_markdown_sections`) with full parameter support
- All 5 extraction functions (`md_extract_code_blocks`, `md_extract_links`, `md_extract_images`, `md_extract_table_rows`, `md_extract_tables_json`)
- Document processing functions (`md_to_html`, `md_to_text`, `md_valid`, `md_stats`, `md_extract_metadata`, `md_extract_section`)
- Advanced section filtering and processing options (min/max level, content inclusion, etc.)
- Frontmatter metadata as `MAP(VARCHAR, VARCHAR)` for easy field access
- Replacement scan support for table-like syntax (`FROM '*.md'`)
- MARKDOWN type with automatic VARCHAR casting
- Cross-platform support (Linux, macOS, WebAssembly, Windows)  
- Robust glob pattern support for local and remote file systems
- High-performance content processing (4,000+ sections/second)
- Comprehensive parameter system for flexible file processing
- Full test suite with 218+ passing assertions

**🗓️ Future Roadmap:**
- Custom renderer integration for specialized markdown flavors
- Streaming parser optimizations for very large documents (>100MB)
- Advanced query optimization for document search workloads
- Optional YAML extension integration for advanced frontmatter parsing

**💡 Note:** DuckDB CLI already supports markdown table output via `.mode markdown`, eliminating the need for COPY TO markdown functionality.

## Dependencies

- **cmark-gfm**: GitHub Flavored Markdown parsing library
- **DuckDB**: Version 1.0.0 or later

## Building

```bash
# Clone with dependencies
git clone --recurse-submodules https://github.com/teaguesterling/duckdb_markdown
cd duckdb_markdown

# Build extension
make

# Run tests
make test
```

## Testing

Comprehensive test suite with 218 passing assertions across 11 test files:

- **Functionality tests**: All extraction functions with edge cases
- **Performance tests**: Large-scale document processing
- **Cross-platform tests**: File system compatibility scenarios
- **Integration tests**: Complex queries and real-world usage patterns

## Contributing

Contributions welcome! The extension provides a solid foundation for Markdown analysis with room for enhancements:

- **File reading functions**: Complete the table function implementations
- **Metadata extraction**: Frontmatter parsing and document statistics  
- **Performance optimizations**: Streaming improvements for very large documents
- **Windows support**: Help resolve remaining platform compatibility issues

## License

MIT License - see LICENSE file for details.
