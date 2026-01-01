# Ecosystem Integration

The DuckDB Markdown extension is part of a document processing ecosystem. This page shows how to use related extensions for cross-format conversion and advanced block manipulation.

## Related Extensions

| Extension | Purpose | Install |
|-----------|---------|---------|
| [webbed](https://github.com/teaguesterling/duckdb_webbed) | HTML/XML processing | `INSTALL webbed FROM community` |
| [duck_block_utils](https://github.com/teaguesterling/duckdb_duck_block_utils) | Block manipulation utilities | `INSTALL duck_block_utils FROM community` |

## Converting Markdown to HTML

Use the `webbed` extension to convert markdown blocks to HTML:

```sql
LOAD markdown;
LOAD webbed;

-- Read markdown and convert to HTML
SELECT doc_blocks_to_html(
    list(b ORDER BY block_order)
)
FROM read_markdown_blocks('README.md') b;
```

### Cross-Format Pipeline

```sql
-- Markdown -> doc_blocks -> HTML
WITH blocks AS (
    SELECT list(b ORDER BY block_order) as doc_blocks
    FROM read_markdown_blocks('input.md') b
)
SELECT doc_blocks_to_html(doc_blocks) as html
FROM blocks;
```

## Converting HTML to Markdown

Convert HTML documents to markdown using the doc_block intermediate format:

```sql
LOAD markdown;
LOAD webbed;

-- HTML -> doc_blocks -> Markdown
SELECT duck_blocks_to_md(
    html_to_doc_blocks('<h1>Title</h1><p>Some paragraph text.</p>')
);
-- Result: # Title
--
-- Some paragraph text.
```

### Batch HTML to Markdown Conversion

```sql
-- Convert multiple HTML files to markdown
COPY (
    SELECT duck_blocks_to_md(html_to_doc_blocks(content))
    FROM read_html_objects('pages/*.html')
) TO 'output.md';
```

## Block Utilities with duck_block_utils

The `duck_block_utils` extension provides format-agnostic utilities for working with document blocks.

### Setup

```sql
LOAD markdown;
LOAD duck_block_utils;
```

### Generate Table of Contents

```sql
-- Extract headings as a TOC
SELECT * FROM doc_blocks_toc(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('README.md') b)
);

-- Format as indented list
SELECT
    repeat('  ', level - 1) || '- ' || title as toc_line,
    id
FROM doc_blocks_toc(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('docs/**/*.md') b)
);
```

### Extract Plain Text

```sql
-- Get plain text content (strips formatting)
SELECT doc_blocks_to_text(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('doc.md') b)
);
```

### Filter Blocks by Type

```sql
-- Keep only headings and code blocks
SELECT unnest(doc_blocks_filter(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('doc.md') b),
    ['heading', 'code']
));

-- Exclude metadata and raw HTML
SELECT unnest(doc_blocks_exclude(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('doc.md') b),
    ['metadata', 'md:html_block']
));
```

### Extract Code Blocks

```sql
-- Get all code blocks with language metadata
SELECT
    language,
    content as code,
    file_path
FROM doc_blocks_code_blocks(
    (SELECT list(b ORDER BY block_order)
     FROM read_markdown_blocks('tutorial/*.md', include_filepath := true) b)
)
WHERE language = 'python';
```

### Validate Document Structure

```sql
-- Check for schema compliance
SELECT doc_blocks_validate(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('doc.md') b)
);

-- Lint for common issues
SELECT * FROM doc_blocks_lint(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('doc.md') b)
)
WHERE severity = 'error';
```

### Block Statistics

```sql
-- Get block type distribution
SELECT * FROM doc_blocks_stats(
    (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('docs/**/*.md') b)
);
```

## Complete Conversion Pipeline

Combine all extensions for powerful document processing:

```sql
LOAD markdown;
LOAD webbed;
LOAD duck_block_utils;

-- Read markdown, filter content, convert to HTML
SELECT doc_blocks_to_html(
    doc_blocks_filter(
        (SELECT list(b ORDER BY block_order) FROM read_markdown_blocks('doc.md') b),
        ['heading', 'paragraph', 'code']
    )
);

-- HTML to cleaned markdown
SELECT duck_blocks_to_md(
    doc_blocks_exclude(
        html_to_doc_blocks(html_content),
        ['md:html_block', 'raw']
    )
)
FROM web_pages;

-- Generate markdown report from multiple sources
WITH all_headings AS (
    SELECT doc_blocks_headings(
        (SELECT list(b ORDER BY block_order)
         FROM read_markdown_blocks('docs/**/*.md') b)
    ) as headings
)
SELECT duck_blocks_to_md(headings) FROM all_headings;
```

## Function Reference

### webbed (HTML/XML)

| Function | Description |
|----------|-------------|
| `html_to_doc_blocks(html)` | Convert HTML to doc_block list |
| `doc_blocks_to_html(blocks)` | Convert doc_blocks to HTML |
| `xml_to_json(xml)` | Convert XML to JSON |
| `json_to_xml(json)` | Convert JSON to XML |
| `to_xml(value)` | Convert any value to XML |

### duck_block_utils

| Function | Description |
|----------|-------------|
| `doc_blocks_filter(blocks, types[])` | Keep only specified block types |
| `doc_blocks_exclude(blocks, types[])` | Remove specified block types |
| `doc_blocks_to_text(blocks)` | Extract plain text content |
| `doc_blocks_toc(blocks)` | Generate table of contents |
| `doc_blocks_headings(blocks)` | Extract heading hierarchy |
| `doc_blocks_code_blocks(blocks)` | Extract code blocks with metadata |
| `doc_blocks_validate(blocks)` | Check schema compliance |
| `doc_blocks_lint(blocks)` | Check for common issues |
| `doc_blocks_stats(blocks)` | Block type statistics |
| `doc_blocks_merge(blocks1, blocks2)` | Merge two block sequences |
