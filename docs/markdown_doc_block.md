# Markdown Document Block Type

This document specifies the block-level document representation used by the DuckDB Markdown extension. This structured representation enables round-trip document processing and provides a foundation for potential interchange with other document format extensions.

## Overview

The markdown extension represents documents as a sequence of block-level elements. Each block is a row with standardized columns, enabling SQL-based document manipulation while preserving document structure.

This design enables:
- **Round-trip processing**: Read markdown → transform with SQL → write back to markdown
- **Block-level queries**: Filter, aggregate, and analyze document structure
- **Programmatic document generation**: Build documents from structured data
- **Format conversion**: Foundation for cross-format document interchange

## Schema

The `read_markdown_blocks()` function returns rows with these columns:

| Column | Type | Description |
|--------|------|-------------|
| `block_type` | VARCHAR | Block type identifier (see Block Types below) |
| `content` | VARCHAR | Primary content of the block |
| `level` | INTEGER | Heading level (1-6), nesting depth, or NULL |
| `encoding` | VARCHAR | Content encoding: `'text'`, `'json'`, `'yaml'` |
| `attributes` | MAP(VARCHAR, VARCHAR) | Type-specific metadata |
| `block_order` | INTEGER | Sequential position in document |
| `file_path` | VARCHAR | Source file (when `include_filepath := true`) |

### Custom Type

A `markdown_doc_block` STRUCT type is also registered for programmatic document construction:

```sql
-- Create a block manually
SELECT {
    block_type: 'heading',
    content: 'Introduction',
    level: 1,
    encoding: 'text',
    attributes: MAP{'id': 'introduction'},
    block_order: 1
}::markdown_doc_block;
```

## Block Types

### Core Block Types

| Type | Description | Content | Level | Common Attributes |
|------|-------------|---------|-------|-------------------|
| `heading` | ATX heading (`#`, `##`, etc.) | Heading text | 1-6 | `id` |
| `paragraph` | Text paragraph | Paragraph text | NULL | - |
| `code` | Fenced code block | Code content | NULL | `language` |
| `blockquote` | Block quotation | Quote content | Nesting depth (1+) | - |
| `list` | Ordered or unordered list | JSON array of items | 1 | `ordered`, `start` |
| `table` | Table | JSON object | NULL | - |
| `hr` | Horizontal rule | Empty | NULL | - |
| `frontmatter` | YAML frontmatter | YAML content | 0 | - |

### Heading

```sql
-- Example from read_markdown_blocks
block_type: 'heading'
content: 'Introduction'
level: 1
encoding: 'text'
attributes: {'id': 'introduction'}
```

Renders as:
```markdown
# Introduction
```

### Paragraph

```sql
block_type: 'paragraph'
content: 'This is a paragraph with **bold** and *italic* text.'
level: NULL
encoding: 'text'
attributes: {}
```

Inline formatting is preserved as-is in the content string.

### Code Block

```sql
block_type: 'code'
content: 'def hello():\n    print("Hello, world!")'
level: NULL
encoding: 'text'
attributes: {'language': 'python'}
```

Renders as:
~~~markdown
```python
def hello():
    print("Hello, world!")
```
~~~

### Blockquote

```sql
block_type: 'blockquote'
content: 'This is a quoted passage.'
level: 1
encoding: 'text'
attributes: {}
```

Renders as:
```markdown
> This is a quoted passage.
```

### List

Lists use JSON encoding to represent items:

```sql
block_type: 'list'
content: '["First item", "Second item", "Third item"]'
level: 1
encoding: 'json'
attributes: {'ordered': 'false'}
```

Renders as:
```markdown
- First item
- Second item
- Third item
```

**Ordered list:**
```sql
block_type: 'list'
content: '["First", "Second", "Third"]'
level: 1
encoding: 'json'
attributes: {'ordered': 'true', 'start': '1'}
```

Renders as:
```markdown
1. First
2. Second
3. Third
```

### Table

Tables use JSON encoding:

```sql
block_type: 'table'
content: '{"headers": ["Name", "Age"], "rows": [["Alice", "30"], ["Bob", "25"]]}'
level: NULL
encoding: 'json'
attributes: {}
```

Renders as:
```markdown
| Name | Age |
|---|---|
| Alice | 30 |
| Bob | 25 |
```

### Horizontal Rule

```sql
block_type: 'hr'
content: ''
level: NULL
encoding: 'text'
attributes: {}
```

Renders as:
```markdown
---
```

### Frontmatter

```sql
block_type: 'frontmatter'
content: 'title: My Document\nauthor: Jane Doe'
level: 0
encoding: 'yaml'
attributes: {}
```

Renders as:
```markdown
---
title: My Document
author: Jane Doe
---
```

Note: `level: 0` indicates frontmatter, consistent with `read_markdown_sections`.

## API

### Reading Blocks

```sql
-- Read markdown file and return one row per block
SELECT block_type, content, level, attributes
FROM read_markdown_blocks('document.md')
ORDER BY block_order;

-- With file path for multi-file queries
SELECT file_path, block_type, content
FROM read_markdown_blocks('docs/**/*.md', include_filepath := true);

-- Filter to specific block types
SELECT content, attributes['language'] as lang
FROM read_markdown_blocks('tutorial.md')
WHERE block_type = 'code';

-- Count blocks by type
SELECT block_type, count(*) as count
FROM read_markdown_blocks('**/*.md')
GROUP BY block_type
ORDER BY count DESC;
```

### Writing Blocks

Use `COPY TO` with `markdown_mode 'blocks'`:

```sql
-- Round-trip: read, transform, write
COPY (
    SELECT block_type, content, level, encoding, attributes
    FROM read_markdown_blocks('input.md')
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- Transform headings to uppercase
COPY (
    SELECT
        block_type,
        CASE WHEN block_type = 'heading' THEN upper(content) ELSE content END as content,
        level,
        encoding,
        attributes
    FROM read_markdown_blocks('doc.md')
    ORDER BY block_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- Generate document from scratch
COPY (
    SELECT * FROM (VALUES
        ('heading', 'My Document', 1, 'text', MAP{}),
        ('paragraph', 'Welcome to this guide.', NULL, 'text', MAP{}),
        ('code', 'print("hello")', NULL, 'text', MAP{'language': 'python'})
    ) AS t(block_type, content, level, encoding, attributes)
) TO 'generated.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

### COPY TO Options

```sql
COPY data TO 'file.md' (FORMAT MARKDOWN,
    markdown_mode 'blocks',
    block_type_column 'block_type',   -- Default: 'block_type'
    content_column 'content',          -- Default: 'content'
    level_column 'level',              -- Default: 'level'
    encoding_column 'encoding',        -- Default: 'encoding'
    attributes_column 'attributes'     -- Default: 'attributes'
);
```

## Examples

### Extract All Code Examples

```sql
-- Get all Python code blocks from documentation
SELECT
    file_path,
    content as code,
    block_order
FROM read_markdown_blocks('docs/**/*.md', include_filepath := true)
WHERE block_type = 'code'
  AND attributes['language'] = 'python'
ORDER BY file_path, block_order;
```

### Document Statistics

```sql
-- Analyze document structure
SELECT
    file_path,
    count(*) FILTER (WHERE block_type = 'heading') as headings,
    count(*) FILTER (WHERE block_type = 'code') as code_blocks,
    count(*) FILTER (WHERE block_type = 'paragraph') as paragraphs,
    count(*) FILTER (WHERE block_type = 'table') as tables
FROM read_markdown_blocks('**/*.md', include_filepath := true)
GROUP BY file_path;
```

### Filter and Reassemble

```sql
-- Remove all code blocks from a document
COPY (
    SELECT block_type, content, level, encoding, attributes
    FROM read_markdown_blocks('doc.md')
    WHERE block_type != 'code'
    ORDER BY block_order
) TO 'doc_no_code.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

### Merge Documents

```sql
-- Combine multiple documents with separators
COPY (
    SELECT block_type, content, level, encoding, attributes
    FROM (
        SELECT *, 1 as doc_order FROM read_markdown_blocks('intro.md')
        UNION ALL
        SELECT 'hr' as block_type, '' as content, NULL as level,
               'text' as encoding, MAP{} as attributes, 999 as block_order, 2 as doc_order
        UNION ALL
        SELECT *, 3 as doc_order FROM read_markdown_blocks('main.md')
    )
    ORDER BY doc_order, block_order
) TO 'combined.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

## Block Rendering Rules

When writing blocks to markdown:

| Block Type | Rendering |
|------------|-----------|
| `heading` | `#` repeated `level` times, followed by content |
| `paragraph` | Content followed by blank line |
| `code` | Fenced with ``` and optional language from `attributes['language']` |
| `blockquote` | Each line prefixed with `>` |
| `list` | JSON array rendered as `- item` or `N. item` based on `attributes['ordered']` |
| `table` | JSON object rendered as markdown table with `\|` separators |
| `hr` | `---` |
| `frontmatter` | Content wrapped in `---` delimiters |

## Round-Trip Fidelity

The block representation preserves document structure but normalizes some formatting:

**Preserved:**
- Block order and hierarchy
- Heading levels
- Code block languages
- List ordering (ordered vs unordered)
- Table structure
- Frontmatter content
- Inline formatting within blocks

**Normalized:**
- Whitespace between blocks (standardized to single blank line)
- Heading style (always ATX `#` style, not Setext underlines)
- Horizontal rule style (always `---`)
- Code fence style (always triple backticks)

## Future: Document Interchange

The block representation is designed to enable document interchange between format-specific extensions. Future extensions could define compatible types:

- `html_doc_block` - HTML documents
- `xml_doc_block` - XML/XHTML documents
- `rst_doc_block` - reStructuredText documents

Converter functions would enable cross-format operations:

```sql
-- Hypothetical future API
SELECT markdown_to_html_blocks(blocks) FROM markdown_docs;
SELECT html_to_markdown_blocks(blocks) FROM html_docs;
```

## Version History

- **1.1** (2024-12): Updated to match flattened output implementation
- **1.0** (2024): Initial specification
