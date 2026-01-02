# Markdown Document Block Implementation

This document describes the Markdown-specific implementation of the [Document Block Specification](doc_block_spec.md).

**Extension**: duckdb_markdown
**Namespace**: `md`
**Spec Version**: 1.0

## Overview

The markdown extension implements the doc_block spec for CommonMark and GitHub Flavored Markdown documents. It provides:

- `read_markdown_blocks()` - Parse markdown into block rows
- `COPY TO` with `markdown_mode 'blocks'` or `'duck_block'` - Render blocks to markdown files
- `duck_block_to_md()` - Convert single block to markdown string
- `duck_blocks_to_md()` - Convert list of blocks to markdown string
- `duck_blocks_to_sections()` - Convert blocks to hierarchical sections
- `doc_element_to_md()` - Convert single doc_element (block or inline) to markdown
- `doc_elements_to_md()` - Convert list of doc_elements to markdown string

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
    markdown_mode 'blocks',  -- or 'duck_block'
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

## Duck Block Conversion Functions

Convert blocks back to Markdown without writing to files. These functions enable in-memory document transformation pipelines.

### duck_block_to_md(block)

Converts a single `markdown_doc_block` struct to a Markdown string.

```sql
SELECT duck_block_to_md({
    block_type: 'heading',
    content: 'Hello World',
    level: 1,
    encoding: 'text',
    attributes: MAP{},
    block_order: 0
}::markdown_doc_block);
-- Returns: '# Hello World\n\n'
```

### duck_blocks_to_md(blocks[])

Converts a list of blocks to a complete Markdown document string.

```sql
-- Transform and render in one query
SELECT duck_blocks_to_md(
    list(b ORDER BY block_order)
)
FROM read_markdown_blocks('source.md') b;

-- Build document programmatically
SELECT duck_blocks_to_md([
    {block_type: 'heading', content: 'Title', level: 1, encoding: 'text', attributes: MAP{}, block_order: 0},
    {block_type: 'paragraph', content: 'Body.', level: NULL, encoding: 'text', attributes: MAP{}, block_order: 1}
]::markdown_doc_block[]);
```

### duck_blocks_to_sections(blocks[])

Converts blocks to a list of section structs with hierarchy information.

**Output Schema:**
| Column | Type | Description |
|--------|------|-------------|
| `section_id` | VARCHAR | Section identifier (from `id` attribute or generated) |
| `section_path` | VARCHAR | Hierarchical path (`"Parent > Child > Grandchild"`) |
| `level` | INTEGER | Heading level (0 for frontmatter) |
| `title` | VARCHAR | Section heading text |
| `content` | MARKDOWN | Rendered content until next heading |

```sql
-- Extract sections from blocks
SELECT s.section_id, s.level, s.title, length(s.content) as len
FROM (
    SELECT unnest(duck_blocks_to_sections(
        list(b ORDER BY block_order)
    )) as s
    FROM read_markdown_blocks('doc.md') b
);

-- Pipeline: read blocks -> filter -> convert to sections
SELECT s.*
FROM (
    SELECT unnest(duck_blocks_to_sections(
        list(b ORDER BY block_order)
    )) as s
    FROM read_markdown_blocks('tutorial.md') b
    WHERE b.block_type IN ('heading', 'paragraph', 'code')
);
```

## Unified Element Functions

The `doc_element` type provides a unified representation for both block and inline elements, enabling format-agnostic document construction.

### doc_element Type

```sql
STRUCT(
    kind          VARCHAR,              -- 'block' or 'inline'
    element_type  VARCHAR,              -- 'heading', 'bold', 'link', etc.
    content       VARCHAR,              -- Text content
    level         INTEGER,              -- Heading level or nesting depth
    encoding      VARCHAR,              -- 'text', 'json', etc.
    attributes    MAP(VARCHAR, VARCHAR),-- Key-value metadata
    element_order INTEGER               -- Position in sequence
)
```

### Supported Element Types

**Block types** (kind = 'block'):
`heading`, `paragraph`, `code`, `blockquote`, `list`, `table`, `hr`, `metadata`, `image`

**Inline types** (kind = 'inline'):

| Type | Markdown Output | Attributes |
|------|-----------------|------------|
| `link` | `[text](href "title")` | `href`, `title` |
| `image` | `![alt](src "title")` | `src`, `title` |
| `bold` / `strong` | `**text**` | - |
| `italic` / `em` | `*text*` | - |
| `code` | `` `text` `` | - |
| `text` | plain text | - |
| `space` | word separator | - |
| `softbreak` | soft line break | - |
| `linebreak` / `br` | hard break | - |
| `strikethrough` / `del` | `~~text~~` | - |
| `superscript` / `sup` | `^text^` | - |
| `subscript` / `sub` | `~text~` | - |
| `math` | `$text$` or `$$text$$` | `display`: inline/block |
| `quoted` | `"text"` or `'text'` | `quote_type`: single/double |
| `cite` | `[@key]` | `key` |
| `note` | `[^note]` | - |

### doc_element_to_md(element)

Converts a single `doc_element` struct to a Markdown string.

```sql
-- Inline link
SELECT doc_element_to_md({
    kind: 'inline',
    element_type: 'link',
    content: 'Click here',
    level: 1,
    encoding: 'text',
    attributes: MAP{'href': 'https://example.com'},
    element_order: 0
}::doc_element);
-- Returns: '[Click here](https://example.com)'

-- Block heading
SELECT doc_element_to_md({
    kind: 'block',
    element_type: 'heading',
    content: 'Title',
    level: 2,
    encoding: 'text',
    attributes: MAP{},
    element_order: 0
}::doc_element);
-- Returns: '## Title\n\n'
```

### doc_elements_to_md(elements[])

Converts a list of elements to a concatenated Markdown string.

```sql
-- Compose rich inline content
SELECT doc_elements_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: 1, encoding: 'text', attributes: MAP{'href': 'https://docs.example.com'}, element_order: 1},
    {kind: 'inline', element_type: 'text', content: ' for ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 2},
    {kind: 'inline', element_type: 'bold', content: 'more info', level: 1, encoding: 'text', attributes: MAP{}, element_order: 3}
]::doc_element[]);
-- Returns: 'Check out [our docs](https://docs.example.com) for **more info**'
```

### Using Elements in Blocks

Combine element functions with block functions for rich document generation:

```sql
-- Paragraph with formatted content
SELECT duck_block_to_md({
    block_type: 'paragraph',
    content: doc_elements_to_md([
        {kind: 'inline', element_type: 'text', content: 'Visit ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
        {kind: 'inline', element_type: 'link', content: 'GitHub', level: 1, encoding: 'text', attributes: MAP{'href': 'https://github.com'}, element_order: 1},
        {kind: 'inline', element_type: 'text', content: ' for projects.', level: 1, encoding: 'text', attributes: MAP{}, element_order: 2}
    ]::doc_element[]),
    level: NULL,
    encoding: 'text',
    attributes: MAP{},
    block_order: 0
}::markdown_doc_block);
-- Returns: 'Visit [GitHub](https://github.com) for projects.\n\n'
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
| 1.3 | 2025-01 | Added unified `doc_element` type with `doc_element_to_md()` and `doc_elements_to_md()` functions |
| 1.2 | 2024-12 | Added duck_block conversion functions, `duck_block` COPY mode alias |
| 1.1 | 2024-12 | Aligned with doc_block_spec v1.0, added metadata type |
| 1.0 | 2024 | Initial implementation |
