# DuckDB Markdown Extension

[![Documentation Status](https://readthedocs.org/projects/duckdb-markdown/badge/?version=latest)](https://duckdb-markdown.readthedocs.io/)

This extension adds Markdown processing capabilities to DuckDB, enabling structured analysis of Markdown documents and content extraction for documentation analysis, content auditing, and knowledge base processing.

## Features

- **Markdown Content Extraction**: Extract code blocks, links, images, and tables from Markdown text
- **COPY TO Markdown**: Export query results as Markdown tables or reconstruct documents from sections
- **Documentation Analysis**: Analyze large documentation repositories with SQL queries
- **Cross-Platform Support**: Works on Linux, macOS, Windows, and WebAssembly (browsers)
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

-- Export results as Markdown table
COPY (SELECT * FROM my_table) TO 'output.md' (FORMAT MARKDOWN);

-- Round-trip: read sections, process, write back
COPY (
  SELECT level, title, upper(content) as content
  FROM read_markdown_sections('doc.md')
) TO 'processed.md' (FORMAT MARKDOWN, markdown_mode 'document');
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
- `include_filepath := false` - Include file_path column in output (alias: `filename`)
- `content_as_varchar := false` - Return content as VARCHAR instead of MARKDOWN type
- `maximum_file_size := 16777216` - Maximum file size in bytes (16MB default)
- `extract_metadata := true` - Extract frontmatter metadata
- `normalize_content := true` - Normalize Markdown content

**Returns:** `(content MARKDOWN, metadata MAP(VARCHAR, VARCHAR))` or `(file_path VARCHAR, content MARKDOWN, metadata MAP(VARCHAR, VARCHAR))` with `include_filepath := true`

#### `read_markdown_blocks(files, [parameters...])`
Reads Markdown files and parses them into block-level elements (headings, paragraphs, code blocks, lists, tables, etc.).

**Parameters:**
- `files` (required) - File path, glob pattern, or list of patterns
- `include_filepath := false` - Include file_path column in output (alias: `filename`)

**Returns:** `(kind VARCHAR, element_type VARCHAR, content VARCHAR, level INTEGER, encoding VARCHAR, attributes MAP(VARCHAR, VARCHAR), element_order INTEGER)`

**Note on level vs heading_level:** For headings, the H1-H6 level is stored in `attributes['heading_level']` (preferred). If not present, the `level` field is used as a fallback.

**Element Types:** `heading`, `paragraph`, `code`, `blockquote`, `list`, `table`, `hr`, `frontmatter`

**Encoding:** `text` for plain content, `json` for structured content (lists, tables), `yaml` for frontmatter

```sql
-- Parse document into blocks
SELECT element_type, content, level
FROM read_markdown_blocks('README.md')
ORDER BY element_order;

-- Extract all code blocks with their languages
SELECT content, attributes['language'] as lang
FROM read_markdown_blocks('docs/**/*.md')
WHERE element_type = 'code';

-- Round-trip: read blocks, modify, write back
COPY (
  SELECT kind, element_type, content, level, encoding, attributes
  FROM read_markdown_blocks('doc.md')
) TO 'copy.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

#### `read_markdown_sections(files, [parameters...])`
Reads Markdown files and parses them into hierarchical sections.

**Parameters:**
- `files` (required) - File path, glob pattern, directory, or list of mixed patterns. Supports fragment syntax: `'file.md#section-id'` to filter to a specific section and its descendants.
- `include_content := true` - Include section content in output
- `min_level := 1` - Minimum heading level to include (1-6)
- `max_level := 6` - Maximum heading level to include (1-6)
- `content_mode := 'minimal'` - Content extraction mode (see below)
- `max_depth := 6` - Maximum depth relative to min_level (e.g., `max_depth := 2` with `min_level := 1` includes h1 and h2 only)
- `max_content_length := 0` - Maximum content length for 'smart' mode (0 = auto, uses 2000 chars)
- `include_empty_sections := false` - Include sections without content
- `include_filepath := false` - Include file_path column in output (alias: `filename`)
- `extract_metadata := true` - Include frontmatter as a special section (level=0)
- Plus all `read_markdown` parameters

**Content Modes:**
- `'minimal'` (default) - Content stops at ANY next heading. Each section contains only its immediate content, not subsections.
- `'full'` - Content includes all subsections until next same-or-higher level heading. Use this for complete section extraction.
- `'smart'` - Adaptive mode: includes small subsections fully, truncates large ones with references like `"... (see #subsection-id)"`.

**Returns:** `(section_id VARCHAR, section_path VARCHAR, level INTEGER, title VARCHAR, content MARKDOWN, parent_id VARCHAR, start_line BIGINT, end_line BIGINT)` or with `include_filepath := true` adds `file_path VARCHAR` column.

**Notes:**
- When `extract_metadata := true`, YAML frontmatter is included as a special section with `level=0`, `section_id='frontmatter'`, and the raw YAML content (without `---` delimiters) as the content.
- The `section_path` column provides hierarchical navigation paths like `"parent/child/grandchild"`.
- Fragment syntax `'file.md#section-id'` returns the matching section and all its descendants.

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
- **`md_extract_section(markdown, section_id, [include_subsections])`** - Extract specific section by ID. With `include_subsections := true`, includes all nested content (full mode); default is minimal mode.
- **`md_extract_sections(markdown, [min_level, max_level, content_mode])`** - Extract all sections as a list. Supports optional level filtering and content_mode ('minimal', 'full', 'smart').
- **`md_section_breadcrumb(markdown, section_id)`** - Generate breadcrumb path for a section (returns "Title1 > Title2 > Title3" format)
- **`value_to_md(value)`** - Convert any value to markdown representation

### Duck Block Functions

Convert document blocks to/from Markdown. These functions work with the `duck_block` structure for in-memory document transformations:

- **`duck_block_to_md(block)`** - Convert a single block or inline element to Markdown string
- **`duck_blocks_to_md(blocks[])`** - Convert a list of blocks to a complete Markdown document
- **`duck_blocks_to_sections(blocks[])`** - Convert blocks to a list of sections with hierarchy

**duck_block structure:**
```sql
STRUCT(
    kind          VARCHAR,              -- 'block' or 'inline'
    element_type  VARCHAR,              -- 'heading', 'paragraph', 'bold', 'link', etc.
    content       VARCHAR,              -- Text content
    level         INTEGER,              -- Document nesting depth (1 for top-level, 0 for frontmatter)
    encoding      VARCHAR,              -- 'text', 'json', 'yaml'
    attributes    MAP(VARCHAR, VARCHAR),-- Element metadata (heading_level, language, href, etc.)
    element_order INTEGER               -- Position in sequence
)
```

**Note:** For headings, the H1-H6 level is stored in `attributes['heading_level']` (preferred). If not present, the `level` field is used as a fallback.

**Supported inline types:** `text`, `bold`/`strong`, `italic`/`em`, `code`, `link`, `image`, `strikethrough`/`del`, `linebreak`/`br`, `math`, `superscript`/`sup`, `subscript`/`sub`

```sql
-- Convert blocks back to markdown
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('source.md') b;

-- Build document programmatically
SELECT duck_blocks_to_md([
    {kind: 'block', element_type: 'heading', content: 'Title', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'block', element_type: 'paragraph', content: 'Body text.', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 1}
]);

-- Compose inline elements
SELECT duck_blocks_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: NULL, encoding: 'text', attributes: MAP{'href': 'https://example.com'}, element_order: 1},
    {kind: 'inline', element_type: 'text', content: ' for details.', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 2}
]);
-- Returns: 'Check out [our docs](https://example.com) for details.'

-- Convert blocks to sections
SELECT s.section_id, s.level, s.title
FROM (
    SELECT unnest(duck_blocks_to_sections(list(b ORDER BY element_order))) as s
    FROM read_markdown_blocks('doc.md') b
);
```

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

### Content Mode Examples

```sql
-- Default 'minimal' mode: each section has only its immediate content
SELECT section_id, title, length(content) as content_length
FROM read_markdown_sections('docs/guide.md')
WHERE level = 1;

-- 'full' mode: sections include all nested subsections
SELECT section_id, title, length(content) as content_length
FROM read_markdown_sections('docs/guide.md', content_mode := 'full')
WHERE level = 1;

-- 'smart' mode: adaptive - includes small subsections, summarizes large ones
SELECT section_id, title, content
FROM read_markdown_sections('docs/guide.md', content_mode := 'smart')
WHERE level = 1;

-- Fragment syntax: get a specific section and its descendants
SELECT section_id, title, level
FROM read_markdown_sections('README.md#installation');

-- Limit parsing depth: only top 2 levels
SELECT section_id, level, title
FROM read_markdown_sections('docs/**/*.md', max_depth := 2);

-- Extract section with subsections using scalar function
SELECT md_extract_section(content, 'api-reference', true) as full_section
FROM read_markdown('docs/api.md');

-- Extract section without subsections (minimal)
SELECT md_extract_section(content, 'api-reference', false) as minimal_section
FROM read_markdown('docs/api.md');

-- Use section_path for hierarchical navigation
SELECT section_path, title
FROM read_markdown_sections('docs/**/*.md')
WHERE section_path LIKE 'api-reference/%';
```

## COPY TO Markdown

Export query results to Markdown files. Three modes support different use cases:

| Mode | Use Case | Input Columns |
|------|----------|---------------|
| `table` (default) | Export any query as a markdown table | Any columns |
| `document` | Reconstruct markdown from sections | `level`, `title`, `content` |
| `blocks` / `duck_block` | Round-trip block-level representation | `kind`, `element_type`, `content`, `level`, `encoding`, `attributes` |

### Table Mode (Default)

Export any query result as a formatted Markdown table with automatic column alignment:

```sql
-- Basic table export
COPY (SELECT * FROM my_table) TO 'output.md' (FORMAT MARKDOWN);

-- With options
COPY my_table TO 'output.md' (FORMAT MARKDOWN,
    header true,           -- Include header row (default: true)
    escape_pipes true,     -- Escape | characters (default: true)
    escape_newlines true,  -- Convert newlines to <br> (default: true)
    null_value 'N/A'       -- Custom NULL representation (default: empty)
);
```

**Output:**
```markdown
| id | name | score |
|---:|---|---:|
| 1 | Alice | 95.5 |
| 2 | Bob | 87.0 |
```

Alignment is automatic: numeric columns are right-aligned, text is left-aligned, booleans are centered.

### Document Mode

Reconstruct Markdown documents from structured section data. This complements `read_markdown_sections` for round-trip document processing:

```sql
-- Create sections
CREATE TABLE sections (level INTEGER, title VARCHAR, content VARCHAR);
INSERT INTO sections VALUES
    (1, 'Introduction', 'Welcome to the guide.'),
    (2, 'Getting Started', 'First steps here.'),
    (1, 'Conclusion', 'Thanks for reading!');

-- Export as document
COPY sections TO 'guide.md' (FORMAT MARKDOWN, markdown_mode 'document');
```

**Output:**
```markdown
# Introduction

Welcome to the guide.

## Getting Started

First steps here.

# Conclusion

Thanks for reading!
```

**Document Mode Options:**
```sql
COPY sections TO 'doc.md' (FORMAT MARKDOWN,
    markdown_mode 'document',
    level_column 'level',      -- Column with heading level 1-6 (default: 'level')
    title_column 'title',      -- Column with heading text (default: 'title')
    content_column 'content',  -- Column with section body (default: 'content')
    frontmatter 'title: My Doc
author: Me',                   -- Optional YAML frontmatter
    blank_lines 1              -- Blank lines between sections (default: 1)
);
```

**Level 0 as Frontmatter:**
Rows with `level = 0` are rendered as YAML frontmatter blocks:

```sql
INSERT INTO doc VALUES (0, '', 'title: Generated Doc');
INSERT INTO doc VALUES (1, 'Body', 'Main content');
COPY doc TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'document');
```

**Output:**
```markdown
---
title: Generated Doc
---

# Body

Main content
```

### Blocks Mode

Export block-level document representation for full-fidelity round-trips. This mode complements `read_markdown_blocks()` and supports both block and inline elements:

```sql
-- Read blocks from a file
CREATE TABLE blocks AS
SELECT kind, element_type, content, level, encoding, attributes
FROM read_markdown_blocks('source.md');

-- Modify blocks
UPDATE blocks SET content = upper(content) WHERE element_type = 'heading';

-- Write back to markdown
COPY blocks TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

**Blocks Mode Options:**
```sql
COPY blocks TO 'doc.md' (FORMAT MARKDOWN,
    markdown_mode 'blocks',
    kind_column 'kind',               -- Column with kind ('block' or 'inline', default: 'kind')
    element_type_column 'element_type',-- Column with element type (default: 'element_type')
    content_column 'content',          -- Column with block content (default: 'content')
    level_column 'level',              -- Column with level/depth (default: 'level')
    encoding_column 'encoding',        -- Column with encoding type (default: 'encoding')
    attributes_column 'attributes'     -- Column with attributes map (default: 'attributes')
);
```

**Block Rendering (`kind = 'block'`):**
- `heading` - Rendered as `# Title` (level determines # count)
- `paragraph` - Plain text with blank lines
- `code` - Fenced code blocks with language from `attributes['language']`
- `blockquote` - Prefixed with `>`
- `list` - JSON array rendered as bullet/numbered list (uses `attributes['ordered']`)
- `table` - JSON object rendered as markdown table
- `hr` - Horizontal rule `---`
- `frontmatter` - YAML block between `---` delimiters

**Inline Rendering (`kind = 'inline'`):**
- `bold` / `strong` - `**text**`
- `italic` / `em` - `*text*`
- `code` - `` `text` ``
- `link` - `[text](href "title")`
- `text` - Plain text (no formatting)
- Plus: `strikethrough`, `superscript`, `subscript`, `math`, etc.

Inline elements are concatenated without trailing newlines. When transitioning from inline to block elements, proper paragraph breaks are automatically inserted.

### Round-Trip Workflow Example

Complete workflow reading, transforming, and writing markdown:

```sql
-- Read sections from source document
CREATE TABLE my_sections AS
SELECT level, title, content
FROM read_markdown_sections('source.md', content_mode := 'minimal');

-- Transform content (e.g., add prefix to all headings)
UPDATE my_sections SET title = 'Chapter: ' || title WHERE level = 1;

-- Write back to markdown
COPY my_sections TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'document');

-- Or use blocks for full-fidelity round-trip
COPY (
    SELECT kind, element_type, content, level, encoding, attributes
    FROM read_markdown_blocks('source.md')
) TO 'copy.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
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

-- Links (is_reference=true for reference-style links like [text][ref])
LIST<STRUCT(text VARCHAR, url VARCHAR, title VARCHAR, is_reference BOOLEAN, line_number BIGINT)>

-- Images
LIST<STRUCT(alt_text VARCHAR, url VARCHAR, title VARCHAR, line_number BIGINT)>

-- Table rows
LIST<STRUCT(table_index BIGINT, row_type VARCHAR, row_index BIGINT, column_index BIGINT, cell_value VARCHAR, line_number BIGINT, num_columns BIGINT, num_rows BIGINT)>

-- Tables JSON
LIST<STRUCT(table_index BIGINT, line_number BIGINT, num_columns BIGINT, num_rows BIGINT, headers VARCHAR[], table_data VARCHAR[][])>

-- Blocks (from read_markdown_blocks)
(kind VARCHAR, element_type VARCHAR, content VARCHAR, level INTEGER, encoding VARCHAR, attributes MAP(VARCHAR, VARCHAR), element_order INTEGER)
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

**✅ Available (v1.3.6):**
- Complete file reading functions (`read_markdown`, `read_markdown_sections`, `read_markdown_blocks`) with full parameter support
- **COPY TO markdown** with table, document, and blocks modes (including inline element support)
- All 5 extraction functions (`md_extract_code_blocks`, `md_extract_links`, `md_extract_images`, `md_extract_table_rows`, `md_extract_tables_json`)
- Document processing functions (`md_to_html`, `md_to_text`, `md_valid`, `md_stats`, `md_extract_metadata`, `md_extract_section`, `md_section_breadcrumb`)
- **Block-level document representation** with `read_markdown_blocks()` and `markdown_mode 'blocks'` / `'duck_block'`
- **Duck block conversion functions** (`duck_block_to_md`, `duck_blocks_to_md`, `duck_blocks_to_sections`)
- **Unified duck_block shape** with `kind` field for block/inline differentiation
- **Content modes** for flexible section extraction: `'minimal'` (default), `'full'`, `'smart'`
- **Fragment syntax** for filtering sections: `'file.md#section-id'`
- **Section hierarchy** with `section_path` column for navigation
- **Reference link detection** in `md_extract_links` (`is_reference` field)
- Advanced section filtering and processing options (min/max level, max_depth, content inclusion, etc.)
- Frontmatter metadata as `MAP(VARCHAR, VARCHAR)` for easy field access
- Replacement scan support for table-like syntax (`FROM '*.md'`)
- MARKDOWN type with automatic VARCHAR casting
- Cross-platform support (Linux, macOS, Windows, WebAssembly)
- Robust glob pattern support for local and remote file systems
- High-performance content processing (4,000+ sections/second)
- Comprehensive parameter system for flexible file processing
- Full test suite with 1102 passing assertions across 20 test files

**🗓️ Future Roadmap:**
- Document interchange format for cross-extension compatibility (HTML, XML, etc.)
- Custom renderer integration for specialized markdown flavors
- Streaming parser optimizations for very large documents (>100MB)
- Advanced query optimization for document search workloads

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

Comprehensive test suite with 1102 passing assertions across 20 test files:

- **Functionality tests**: All extraction functions with edge cases
- **Block-level tests**: Round-trip parsing and rendering with inline element support
- **Performance tests**: Large-scale document processing
- **Cross-platform tests**: File system compatibility scenarios
- **Integration tests**: Complex queries and real-world usage patterns

## Contributing

Contributions welcome! The extension provides a solid foundation for Markdown analysis with room for enhancements:

- **File reading functions**: Complete the table function implementations
- **Metadata extraction**: Frontmatter parsing and document statistics  
- **Performance optimizations**: Streaming improvements for very large documents
- **Advanced features**: Custom renderer integration and streaming parser optimizations

## WebAssembly (WASM) Support

The extension fully supports WebAssembly environments (DuckDB-WASM in browsers). All markdown functions work in the browser:

```javascript
// Load the extension in DuckDB-WASM
await conn.query("LOAD 'markdown.duckdb_extension.wasm'");

// Register a markdown file
await db.registerFileText('doc.md', '# Hello\n\nWorld');

// Query markdown content
const result = await conn.query("SELECT * FROM read_markdown('doc.md')");
```

**Important**: Use the extension version that matches your duckdb-wasm version:
- duckdb-wasm 1.32.0 → Use `markdown-v1.4.3-extension-wasm_eh`
- duckdb-wasm latest → Use `markdown-main-extension-wasm_eh`

For technical details on how WASM support was implemented (including cmark-gfm static linking and function pointer compatibility), see [docs/WASM.md](docs/WASM.md).

## License

MIT License - see LICENSE file for details.
