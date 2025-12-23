# Markdown Document Block Type

This document specifies the `markdown_doc_block` type, a structured representation of Markdown document content that enables round-trip document processing and potential interchange with other document format extensions.

## Overview

The `markdown_doc_block` type represents a single block-level element in a Markdown document. A complete document is represented as `LIST<markdown_doc_block>`, preserving the sequential order of blocks.

This design enables:
- **Round-trip processing**: Read markdown, transform with SQL, write back to markdown
- **Format conversion**: Other extensions (HTML, XML, etc.) can define compatible block types with converter macros
- **Programmatic document generation**: Build documents from structured data

## Type Definition

```sql
CREATE TYPE markdown_doc_block AS STRUCT (
    block_type VARCHAR,                   -- Block type identifier
    content VARCHAR,                      -- Primary content
    level INTEGER,                        -- Heading level (1-6), nesting depth, or NULL
    encoding VARCHAR,                     -- Content encoding hint
    attributes MAP(VARCHAR, VARCHAR),     -- Type-specific attributes
    block_order INTEGER                   -- Optional ordering for table storage
);
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `block_type` | VARCHAR | Identifies the block type (see Block Types below) |
| `content` | VARCHAR | Primary content of the block |
| `level` | INTEGER | Heading level (1-6), nesting depth for lists/blockquotes, or NULL |
| `encoding` | VARCHAR | How to interpret `content`: `'text'` (default), `'json'`, `'yaml'`, `'base64'` |
| `attributes` | MAP(VARCHAR, VARCHAR) | Type-specific metadata (language, id, class, etc.) |
| `block_order` | INTEGER | Optional field for maintaining order when stored in tables |

## Block Types

### Core Block Types

| Type | Description | Content | Level | Common Attributes |
|------|-------------|---------|-------|-------------------|
| `heading` | ATX heading (`#`, `##`, etc.) | Heading text | 1-6 | `id`, `class` |
| `paragraph` | Text paragraph | Paragraph text | NULL | `class` |
| `code` | Fenced code block | Code content | NULL | `language`, `id`, `class` |
| `blockquote` | Block quotation | Quote content | Nesting depth (1+) | `class` |
| `list` | Ordered or unordered list | JSON-encoded items | Nesting depth (1+) | `ordered`, `start` |
| `table` | Table | JSON-encoded table data | NULL | `alignments` |
| `image` | Image (block-level) | Image URL or base64 | NULL | `alt`, `title` |
| `hr` | Horizontal rule | NULL or empty | NULL | - |
| `html` | Raw HTML block | HTML content | NULL | - |
| `raw` | Raw content (pass-through) | Any content | NULL | `format` |
| `frontmatter` | YAML frontmatter | YAML content | 0 | Any metadata keys |

### Heading

```sql
{
    block_type: 'heading',
    content: 'Introduction',
    level: 1,
    encoding: 'text',
    attributes: {'id': 'introduction'},
    block_order: NULL
}
```

Renders as:
```markdown
# Introduction
```

### Paragraph

```sql
{
    block_type: 'paragraph',
    content: 'This is a paragraph with **bold** and *italic* text.',
    level: NULL,
    encoding: 'text',
    attributes: {},
    block_order: NULL
}
```

Inline formatting is preserved as-is in the content string.

### Code Block

```sql
{
    block_type: 'code',
    content: 'def hello():\n    print("Hello, world!")',
    level: NULL,
    encoding: 'text',
    attributes: {'language': 'python'},
    block_order: NULL
}
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
{
    block_type: 'blockquote',
    content: 'This is a quoted passage.\n\nWith multiple paragraphs.',
    level: 1,
    encoding: 'text',
    attributes: {},
    block_order: NULL
}
```

Renders as:
```markdown
> This is a quoted passage.
>
> With multiple paragraphs.
```

For nested blockquotes, use `level: 2`, `level: 3`, etc.

### List

Lists use JSON encoding to represent structure:

```sql
{
    block_type: 'list',
    content: '["First item", "Second item", "Third item"]',
    level: 1,
    encoding: 'json',
    attributes: {'ordered': 'false'},
    block_order: NULL
}
```

Renders as:
```markdown
- First item
- Second item
- Third item
```

**Ordered list:**
```sql
{
    block_type: 'list',
    content: '["First", "Second", "Third"]',
    level: 1,
    encoding: 'json',
    attributes: {'ordered': 'true', 'start': '1'},
    block_order: NULL
}
```

**Nested list (JSON structure):**
```json
{
    "items": [
        "First item",
        {"text": "Second item", "children": ["Nested a", "Nested b"]},
        "Third item"
    ]
}
```

### Table

Tables use JSON encoding:

```sql
{
    block_type: 'table',
    content: '{"headers": ["Name", "Age", "City"], "rows": [["Alice", "30", "NYC"], ["Bob", "25", "LA"]]}',
    level: NULL,
    encoding: 'json',
    attributes: {'alignments': 'left,right,left'},
    block_order: NULL
}
```

**JSON Schema for tables:**
```json
{
    "headers": ["Column1", "Column2", ...],
    "rows": [
        ["cell1", "cell2", ...],
        ["cell1", "cell2", ...]
    ]
}
```

Alignments are specified in attributes as comma-separated values: `left`, `right`, `center`.

Renders as:
```markdown
| Name | Age | City |
|---|---:|---|
| Alice | 30 | NYC |
| Bob | 25 | LA |
```

### Image

```sql
{
    block_type: 'image',
    content: 'https://example.com/image.png',
    level: NULL,
    encoding: 'text',
    attributes: {'alt': 'Example image', 'title': 'Figure 1'},
    block_order: NULL
}
```

Renders as:
```markdown
![Example image](https://example.com/image.png "Figure 1")
```

For embedded images, use `encoding: 'base64'` and include the data URI in content.

### Horizontal Rule

```sql
{
    block_type: 'hr',
    content: NULL,
    level: NULL,
    encoding: 'text',
    attributes: {},
    block_order: NULL
}
```

Renders as:
```markdown
---
```

### Frontmatter

```sql
{
    block_type: 'frontmatter',
    content: 'title: My Document\nauthor: Jane Doe\ndate: 2024-01-15',
    level: 0,
    encoding: 'yaml',
    attributes: {},
    block_order: NULL
}
```

Renders as:
```markdown
---
title: My Document
author: Jane Doe
date: 2024-01-15
---
```

Note: `level: 0` indicates frontmatter, consistent with `read_markdown_sections`.

## API

### Reading Blocks

```sql
-- Read markdown file(s) and return blocks
SELECT * FROM read_markdown_blocks('document.md');

-- Returns:
-- file_path VARCHAR (optional)
-- blocks LIST<markdown_doc_block>
-- metadata MAP(VARCHAR, VARCHAR)

-- Unnest for block-level operations
SELECT b.block_type, b.content, b.attributes['language'] as lang
FROM read_markdown_blocks('doc.md'), UNNEST(blocks) AS b
WHERE b.block_type = 'code';
```

### Writing Blocks

```sql
-- COPY with blocks mode
COPY (SELECT blocks FROM my_docs)
TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- From a table with block columns
CREATE TABLE doc_blocks AS
SELECT * FROM UNNEST([
    {block_type: 'heading', content: 'Title', level: 1, encoding: 'text', attributes: {}, block_order: 1},
    {block_type: 'paragraph', content: 'Content here.', level: NULL, encoding: 'text', attributes: {}, block_order: 2}
]::LIST<markdown_doc_block>);

COPY doc_blocks TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

### Scalar Functions

```sql
-- Convert markdown string to blocks
SELECT md_to_blocks(content) FROM documents;

-- Convert blocks to markdown string
SELECT md_from_blocks(blocks) FROM my_docs;
```

## Document Interchange

The `markdown_doc_block` type is designed to enable document interchange between format-specific extensions. Each extension defines its own block type:

- `markdown_doc_block` - This extension (Markdown)
- `html_doc_block` - Hypothetical HTML extension
- `xml_doc_block` - Hypothetical XML extension

### Converter Macros

Extensions can provide converter macros for interoperability:

```sql
-- Hypothetical: Convert HTML blocks to Markdown blocks
SELECT html_to_markdown_blocks(html_blocks) FROM html_docs;

-- Hypothetical: Convert Markdown blocks to HTML blocks
SELECT markdown_to_html_blocks(md_blocks) FROM markdown_docs;
```

### Common Block Type Mapping

| Concept | markdown_doc_block | html_doc_block (hypothetical) |
|---------|-------------------|-------------------------------|
| Heading | `heading` (level 1-6) | `h1`-`h6` |
| Paragraph | `paragraph` | `p` |
| Code | `code` | `pre > code` |
| Quote | `blockquote` | `blockquote` |
| List | `list` | `ul`, `ol` |
| Table | `table` | `table` |
| Image | `image` | `img` |
| Divider | `hr` | `hr` |

## Examples

### Complete Document

```sql
SELECT [
    {block_type: 'frontmatter', content: 'title: Guide\nauthor: Alice', level: 0, encoding: 'yaml', attributes: {}, block_order: 1},
    {block_type: 'heading', content: 'Introduction', level: 1, encoding: 'text', attributes: {'id': 'introduction'}, block_order: 2},
    {block_type: 'paragraph', content: 'Welcome to this guide.', level: NULL, encoding: 'text', attributes: {}, block_order: 3},
    {block_type: 'heading', content: 'Getting Started', level: 2, encoding: 'text', attributes: {'id': 'getting-started'}, block_order: 4},
    {block_type: 'paragraph', content: 'First, install the package:', level: NULL, encoding: 'text', attributes: {}, block_order: 5},
    {block_type: 'code', content: 'pip install example', level: NULL, encoding: 'text', attributes: {'language': 'bash'}, block_order: 6},
    {block_type: 'heading', content: 'Conclusion', level: 1, encoding: 'text', attributes: {'id': 'conclusion'}, block_order: 7},
    {block_type: 'paragraph', content: 'Thanks for reading!', level: NULL, encoding: 'text', attributes: {}, block_order: 8}
]::LIST<markdown_doc_block> as document;
```

### Round-Trip Processing

```sql
-- Read, transform, write
COPY (
    SELECT list_transform(blocks, b ->
        CASE
            WHEN b.block_type = 'heading'
            THEN struct_pack(
                block_type := b.block_type,
                content := upper(b.content),
                level := b.level,
                encoding := b.encoding,
                attributes := b.attributes,
                block_order := b.block_order
            )::markdown_doc_block
            ELSE b
        END
    ) as blocks
    FROM read_markdown_blocks('input.md')
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

### Filter and Extract

```sql
-- Extract all code blocks with their context
WITH doc AS (
    SELECT UNNEST(blocks) as b,
           generate_subscripts(blocks, 1) as idx
    FROM read_markdown_blocks('tutorial.md')
)
SELECT
    b.content as code,
    b.attributes['language'] as language
FROM doc
WHERE b.block_type = 'code';
```

### Generate Documentation

```sql
-- Generate markdown from database schema
SELECT list_aggregate([
    {block_type: 'heading', content: table_name, level: 2, encoding: 'text', attributes: {}, block_order: NULL},
    {block_type: 'paragraph', content: 'Columns:', level: NULL, encoding: 'text', attributes: {}, block_order: NULL},
    {block_type: 'table', content: json_object(
        'headers', ['Name', 'Type', 'Nullable'],
        'rows', list_transform(columns, c -> [c.name, c.type, c.nullable::VARCHAR])
    ), level: NULL, encoding: 'json', attributes: {}, block_order: NULL}
]::LIST<markdown_doc_block>, 'list_concat') as blocks
FROM information_schema.tables
JOIN information_schema.columns USING (table_name);
```

## Compatibility Notes

- **Inline formatting**: Preserved as-is in content strings (e.g., `**bold**`, `*italic*`, `[links](url)`)
- **HTML in Markdown**: Use `block_type: 'html'` for raw HTML blocks
- **Unknown block types**: Writers should pass through unknown types using `raw` with appropriate encoding
- **Empty content**: Use empty string `''` rather than NULL for blocks with no content (except `hr`)

## Version History

- **1.0** (2024): Initial specification
