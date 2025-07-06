# Markdown Extension for DuckDB

This extension enables DuckDB to read and analyze Markdown files directly, providing comprehensive document intelligence capabilities for technical documentation, README files, and other Markdown content at scale.

## Installation

### From Community Extensions

```sql
INSTALL markdown FROM community;
LOAD markdown;
```

### From Source

```bash
git clone https://github.com/your-org/duckdb_markdown
cd duckdb_markdown
make
make test
```

## Key Features

- **Document Reading**: Read Markdown files into DuckDB tables with `read_markdown` and `read_markdown_sections`
- **Section-Based Analysis**: Parse documents into hierarchical sections with stable IDs
- **Full-Text Search Ready**: Convert Markdown to plain text optimized for FTS indexing
- **Content Extraction**: Extract code blocks, links, images, and metadata
- **Native MD Type**: Full Markdown type support with conversion functions
- **Documentation Intelligence**: Calculate statistics, validate links, analyze structure

## Quick Start

```sql
-- Load the extension
LOAD markdown;

-- Read whole documents
SELECT * FROM read_markdown('docs/*.md');

-- Parse into sections for detailed analysis  
SELECT * FROM read_markdown_sections('docs/*.md', 
    min_level=1, 
    max_level=3,
    include_content=true
);

-- Direct file querying
SELECT * FROM 'README.md';

-- Extract searchable text for FTS
SELECT 
    file_path,
    section_id, 
    title,
    md_to_text(content) as searchable_text
FROM read_markdown_sections('docs/*.md');
```

## Core Functions

### Reading Functions

#### `read_markdown(path, [options])`
Returns one row per file with complete document content.

```sql
-- Basic usage
SELECT * FROM read_markdown('docs/*.md');
-- Returns: file_path, content (MD type), metadata (JSON)

-- With options
SELECT * FROM read_markdown('docs/*.md',
    extract_metadata=true,
    include_stats=true,
    flavor='gfm'
);
```

#### `read_markdown_sections(path, [options])`
Returns one row per section for detailed document analysis.

```sql
-- Parse documents into sections
SELECT * FROM read_markdown_sections('docs/*.md',
    include_content=true,    -- Include section content
    min_level=1,             -- Minimum heading level  
    max_level=3,             -- Maximum heading level
    flavor='gfm'             -- GitHub Flavored Markdown
);
-- Returns: file_path, section_id, level, title, content, parent_id, position
```

### Conversion Functions

```sql
-- Convert Markdown to HTML
SELECT md_to_html(content) FROM read_markdown('README.md');

-- Convert to plain text (for FTS)
SELECT md_to_text(content) FROM read_markdown('docs/*.md');

-- Create Markdown from values
SELECT value_to_md({'title': 'Hello', 'content': 'World'});

-- Validate Markdown
SELECT md_valid(content) FROM user_documents;
```

### Content Extraction

```sql
-- Extract code blocks
SELECT 
    file_path,
    code.*
FROM read_markdown_sections('docs/*.md') s
CROSS JOIN LATERAL md_extract_code_blocks(s.content, 'sql') code;

-- Extract all links
SELECT 
    file_path,
    link.text,
    link.url
FROM read_markdown('docs/*.md') d
CROSS JOIN LATERAL md_extract_links(d.content) link;

-- Extract metadata from frontmatter
SELECT 
    file_path,
    md_extract_metadata(content) as metadata
FROM read_markdown('blog/*.md');

-- Calculate document statistics
SELECT 
    file_path,
    md_stats(content) as stats
FROM read_markdown('docs/*.md');
```

## Advanced Use Cases

### Documentation Search System

```sql
-- Build searchable documentation database
CREATE TABLE docs_search AS
SELECT 
    file_path,
    section_id,
    level,
    title,
    md_to_text(content) as body,
    md_section_breadcrumb(file_path, section_id) as breadcrumb
FROM read_markdown_sections('docs/**/*.md', max_level=3);

-- Create FTS index (requires fts extension)
PRAGMA create_fts_index('docs_search', 'title', 'body');

-- Search documentation
SELECT 
    breadcrumb,
    snippet(docs_search, 'body') as match
FROM docs_search
WHERE body MATCH 'connection pooling'
ORDER BY rank
LIMIT 10;
```

### Code Example Database

```sql
-- Extract all code examples with context
CREATE TABLE code_examples AS
SELECT 
    s.file_path,
    md_section_breadcrumb(s.file_path, s.section_id) as location,
    c.language,
    c.code,
    c.line_number
FROM read_markdown_sections('**/*.md') s
CROSS JOIN LATERAL md_extract_code_blocks(s.content) c
WHERE c.language IN ('sql', 'python', 'javascript');

-- Find SQL examples
SELECT 
    location,
    code
FROM code_examples
WHERE language = 'sql'
  AND code ILIKE '%CREATE TABLE%';
```

### Documentation Quality Analysis

```sql
-- Analyze documentation quality
CREATE VIEW doc_quality AS
SELECT 
    file_path,
    stats.word_count,
    stats.heading_count,
    stats.reading_time_minutes,
    COUNT(DISTINCT c.language) as code_languages,
    COUNT(l.url) as total_links,
    SUM(CASE WHEN l.url LIKE 'http:%' THEN 1 ELSE 0 END) as insecure_links
FROM read_markdown('docs/*.md') d
LEFT JOIN LATERAL md_stats(d.content) stats ON true
LEFT JOIN LATERAL md_extract_code_blocks(d.content) c ON true  
LEFT JOIN LATERAL md_extract_links(d.content) l ON true
GROUP BY file_path, stats.word_count, stats.heading_count, stats.reading_time_minutes;
```

### Link Validation

```sql
-- Find broken internal links
WITH docs AS (
    SELECT file_path, content FROM read_markdown('docs/*.md')
),
links AS (
    SELECT 
        d.file_path,
        l.text,
        l.url
    FROM docs d
    CROSS JOIN LATERAL md_extract_links(d.content) l
    WHERE l.url LIKE '#%'
),
headings AS (
    SELECT 
        file_path,
        section_id
    FROM read_markdown_sections('docs/*.md')
)
SELECT DISTINCT
    l.file_path,
    l.text as link_text,
    l.url as broken_link
FROM links l
LEFT JOIN headings h ON h.file_path = l.file_path 
                   AND h.section_id = SUBSTR(l.url, 2)
WHERE h.section_id IS NULL;
```

## Function Reference

### Reading Functions
- `read_markdown(path, [options])` - Read whole documents
- `read_markdown_sections(path, [options])` - Parse into sections

### Conversion Functions  
- `md_to_html(markdown)` - Convert to HTML
- `md_to_text(markdown)` - Convert to plain text
- `value_to_md(value)` - Convert value to Markdown
- `md_valid(text)` - Validate Markdown syntax

### Extraction Functions
- `md_extract_metadata(markdown)` - Extract frontmatter as JSON
- `md_extract_code_blocks(markdown, [language])` - Extract code blocks
- `md_extract_links(markdown)` - Extract links
- `md_extract_images(markdown)` - Extract images  
- `md_extract_headings(markdown, [max_level])` - Extract headings for TOC
- `md_stats(markdown)` - Calculate document statistics

### Section Functions
- `md_extract_section(markdown, section_id)` - Extract specific section
- `md_section_breadcrumb(file_path, section_id)` - Generate breadcrumb path

## Parameters

### Reading Options
- `extract_metadata` (bool): Extract frontmatter metadata (default: true)
- `include_stats` (bool): Calculate document statistics (default: false)
- `flavor` (string): Markdown flavor - 'gfm', 'commonmark' (default: 'gfm')
- `maximum_file_size` (int): Maximum file size in bytes (default: 16MB)

### Section Options  
- `include_content` (bool): Include section content (default: true)
- `min_level` (int): Minimum heading level (default: 1)
- `max_level` (int): Maximum heading level (default: 6)
- `include_empty_sections` (bool): Include sections without content (default: false)

## File Support

- **Extensions**: `.md`, `.markdown`
- **Local files**: `docs/file.md`, `docs/*.md`, `docs/`
- **Remote files**: `https://raw.githubusercontent.com/org/repo/main/README.md`
- **Cloud storage**: `s3://bucket/docs/*.md` (with appropriate extensions)

## Integration

### Full-Text Search
Works seamlessly with DuckDB's FTS extension:

```sql
-- Prepare documents for search
CREATE TABLE searchable AS
SELECT md_to_text(content) as text FROM read_markdown('docs/*.md');

PRAGMA create_fts_index('searchable', 'text');
```

### Vector Search
Combine with vector extensions for semantic search:

```sql
-- Generate embeddings per section
SELECT 
    section_id,
    embedding_function(md_to_text(content)) as vector
FROM read_markdown_sections('docs/*.md');
```

## Performance

- **Streaming**: Large documents are processed efficiently
- **TOC Mode**: Use `include_content=false` for fast table-of-contents generation
- **Lazy Loading**: Section content loaded on demand
- **Batch Processing**: Optimized for processing many files

## Building from Source

```bash
# Clone with dependencies  
git clone --recurse-submodules https://github.com/your-org/duckdb_markdown
cd duckdb_markdown

# Install dependencies (Ubuntu/Debian)
sudo apt-get install libcmark-gfm-dev libcmark-gfm-extensions-dev

# Build
make

# Test
make test
```

## Dependencies

- **cmark-gfm**: GitHub Flavored Markdown parsing
- **DuckDB**: Version 0.9.0 or later

## Roadmap

- [ ] Advanced path expressions (like JSONPath for sections)
- [ ] Markdown modification functions  
- [ ] Schema validation for structured documents
- [ ] Performance optimizations for very large files
- [ ] Extended Markdown flavor support

## License

MIT License - see LICENSE file for details.

## Contributing

Contributions welcome! Please see CONTRIBUTING.md for guidelines.
