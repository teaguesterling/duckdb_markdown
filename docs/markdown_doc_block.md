# Markdown Document Block Implementation

This document describes the Markdown-specific implementation of the [Document Block Specification](doc_block_spec.md).

**Extension**: duckdb_markdown
**Namespace**: `md`
**Spec Version**: 1.0

## Overview

The markdown extension implements the doc_block spec for CommonMark and GitHub Flavored Markdown documents. It provides:

- `read_markdown_blocks()` - Parse markdown into block rows
- `COPY TO` with `markdown_mode 'blocks'` - Render blocks to markdown

## Supported Block Types

### Core Types (from spec)

| Type | Markdown Element | Notes |
|------|------------------|-------|
| `heading` | `#`, `##`, etc. (ATX style) | Setext headings converted to ATX |
| `paragraph` | Text blocks | Inline formatting preserved |
| `code` | Fenced code blocks | Language in `attributes['language']` |
| `blockquote` | `>` quoted blocks | Level = nesting depth |
| `list` | `-`, `*`, `1.` lists | JSON array of items |
| `table` | GFM tables | JSON `{headers, rows}` |
| `hr` | `---`, `***`, `___` | Normalized to `---` on output |
| `metadata` | YAML frontmatter | Level 0, encoding 'yaml' |
| `image` | `![alt](src "title")` | Details in attributes |

### Markdown-Specific Types

| Type | Element | Description |
|------|---------|-------------|
| `md:html_block` | Raw HTML | Preserved HTML in markdown |
| `md:footnote` | `[^id]: text` | Footnote definitions |

## Reader: read_markdown_blocks()

```sql
read_markdown_blocks(
    path VARCHAR,                    -- File path or glob pattern
    include_filepath := false,       -- Add file_path column
    include_raw_html := false        -- Include raw HTML blocks
)
```

### Output Schema

| Column | Type | Description |
|--------|------|-------------|
| `block_type` | VARCHAR | Block type identifier |
| `content` | VARCHAR | Block content |
| `level` | INTEGER | Heading level or nesting depth |
| `encoding` | VARCHAR | 'text', 'json', or 'yaml' |
| `attributes` | MAP(VARCHAR, VARCHAR) | Block metadata |
| `block_order` | INTEGER | Position in document |
| `file_path` | VARCHAR | Source file (when enabled) |

### Examples

```sql
-- Basic usage
SELECT * FROM read_markdown_blocks('README.md');

-- Filter to headings only
SELECT content, level
FROM read_markdown_blocks('doc.md')
WHERE block_type = 'heading'
ORDER BY block_order;

-- Extract code blocks by language
SELECT content, attributes['language'] as lang
FROM read_markdown_blocks('tutorial.md')
WHERE block_type = 'code';

-- Multi-file analysis
SELECT file_path, block_type, count(*) as count
FROM read_markdown_blocks('docs/**/*.md', include_filepath := true)
GROUP BY file_path, block_type;
```

## Writer: COPY TO Blocks

```sql
COPY query TO 'output.md' (
    FORMAT MARKDOWN,
    markdown_mode 'blocks',
    -- Column mapping (defaults shown)
    block_type_column 'block_type',
    content_column 'content',
    level_column 'level',
    encoding_column 'encoding',
    attributes_column 'attributes'
);
```

### Rendering Rules

| Block Type | Markdown Output |
|------------|-----------------|
| `heading` | `#` × level + space + content |
| `paragraph` | content + blank line |
| `code` | ` ``` ` + language + newline + content + ` ``` ` |
| `blockquote` | `>` prefix per line |
| `list` | `- ` or `N. ` per item |
| `table` | `\| cell \|` format with separator |
| `hr` | `---` |
| `metadata` | `---` + content + `---` |
| `image` | `![alt](src "title")` |

### Examples

```sql
-- Round-trip a document
COPY (
    SELECT block_type, content, level, encoding, attributes
    FROM read_markdown_blocks('input.md')
    ORDER BY block_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- Transform headings
COPY (
    SELECT
        block_type,
        CASE WHEN block_type = 'heading'
             THEN upper(content)
             ELSE content END as content,
        level, encoding, attributes
    FROM read_markdown_blocks('doc.md')
    ORDER BY block_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- Generate from scratch
COPY (
    SELECT * FROM (VALUES
        ('metadata', 'title: Generated Doc', 0, 'yaml', MAP{}),
        ('heading', 'Introduction', 1, 'text', MAP{'id': 'intro'}),
        ('paragraph', 'Welcome to this guide.', NULL, 'text', MAP{}),
        ('code', 'print("hello")', NULL, 'text', MAP{'language': 'python'})
    ) AS t(block_type, content, level, encoding, attributes)
) TO 'generated.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

## Round-Trip Fidelity

### Preserved
- Block order and structure
- Heading levels (1-6)
- Code block languages
- List ordering (ordered vs unordered)
- Table structure (headers and rows)
- Frontmatter content
- Inline formatting within blocks

### Normalized
- Whitespace between blocks → single blank line
- Heading style → ATX (`#`) only
- Horizontal rule style → `---`
- Code fence style → triple backticks
- List markers → `-` for unordered, `1.` for ordered

## Interoperability

### Converting from HTML

```sql
-- Future: with duckdb_webbed extension
COPY (
    SELECT
        CASE block_type
            WHEN 'html:h1' THEN 'heading'
            WHEN 'html:h2' THEN 'heading'
            WHEN 'html:p' THEN 'paragraph'
            WHEN 'html:pre' THEN 'code'
            ELSE block_type
        END as block_type,
        content,
        CASE WHEN block_type LIKE 'html:h%'
             THEN CAST(right(block_type, 1) AS INTEGER)
             ELSE level END as level,
        encoding,
        attributes
    FROM read_html_blocks('page.html')
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

### Cross-Format Queries

```sql
-- Future: unified block queries
SELECT block_type, content
FROM (
    SELECT *, 'markdown' as source FROM read_markdown_blocks('docs/*.md')
    UNION ALL
    SELECT *, 'html' as source FROM read_html_blocks('pages/*.html')
)
WHERE block_type = 'heading';
```

## Comparison: Sections vs Blocks

The extension provides two document representations:

| Aspect | read_markdown_sections | read_markdown_blocks |
|--------|------------------------|----------------------|
| Granularity | Hierarchical sections | Flat block sequence |
| Use case | Document navigation | Document transformation |
| Content | Section content (configurable) | Individual blocks |
| Hierarchy | Parent/child relationships | Sequential order only |
| Round-trip | Lossy (section-level) | High fidelity |

**Choose sections** for: TOC generation, section extraction, hierarchical queries
**Choose blocks** for: Document transformation, format conversion, block-level analysis

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.1 | 2024-12 | Aligned with doc_block_spec v1.0, added metadata type |
| 1.0 | 2024 | Initial implementation |
