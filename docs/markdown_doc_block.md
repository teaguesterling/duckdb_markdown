# Markdown Document Block Implementation

This document describes the Markdown-specific implementation of the [Document Block Specification](doc_block_spec.md).

**Extension**: duckdb_markdown
**Namespace**: `md`
**Spec Version**: 2.0

## Overview

The markdown extension implements the duck_block spec for CommonMark and GitHub Flavored Markdown documents. It provides:

- `read_markdown_blocks()` - Parse markdown into block rows (duck_block shape)
- `COPY TO` with `markdown_mode 'blocks'` or `'duck_block'` - Render blocks to markdown files
- `duck_block_to_md()` - Convert single duck_block to markdown string
- `duck_blocks_to_md()` - Convert list of duck_blocks to markdown string
- `duck_blocks_to_sections()` - Convert blocks to hierarchical sections

## Duck Block Type

The extension uses the `duck_block` struct shape from the `duck_block_utils` specification:

```sql
STRUCT(
    kind          VARCHAR,              -- 'block' or 'inline'
    element_type  VARCHAR,              -- 'heading', 'paragraph', 'bold', 'link', etc.
    content       VARCHAR,              -- Text content
    level         INTEGER,              -- Document nesting depth (1 for top-level)
    encoding      VARCHAR,              -- 'text', 'json', 'yaml', 'html', 'xml'
    attributes    MAP(VARCHAR, VARCHAR),-- Key-value metadata (e.g., heading_level, language)
    element_order INTEGER               -- Position in sequence
)
```

**Note:** The `duck_block` type is defined by the `duck_block_utils` extension. This extension uses the shape but does not register the type name.

## Supported Element Types

### Block Types (kind = 'block')

| element_type | Markdown Element | Notes |
|--------------|------------------|-------|
| `heading` | `#`, `##`, etc. (ATX style) | Level in `attributes['heading_level']` (1-6), falls back to `level` field |
| `paragraph` | Text blocks | Inline formatting preserved |
| `code` | Fenced code blocks | Language in `attributes['language']` |
| `blockquote` | `>` quoted blocks | Level = nesting depth |
| `list` | `-`, `*`, `1.` lists | JSON array of items |
| `table` | GFM tables | JSON `{headers, rows}` |
| `hr` | `---`, `***`, `___` | Normalized to `---` on output |
| `metadata` | YAML frontmatter | Level 0, encoding 'yaml' |
| `frontmatter` | YAML frontmatter | Alias for `metadata` |
| `image` | `![alt](src "title")` | Details in attributes |
| `raw` | Raw HTML | Preserved HTML in markdown |
| `html` | Raw HTML | Alias for raw |
| `md:html_block` | Raw HTML | Markdown-specific namespace |

### Inline Types (kind = 'inline')

| element_type | Markdown Output | Attributes |
|--------------|-----------------|------------|
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
| `underline` | `<u>text</u>` | HTML fallback |
| `smallcaps` | `<span style="...">` | HTML fallback |
| `math` | `$text$` or `$$text$$` | `display`: inline/block |
| `quoted` | `"text"` or `'text'` | `quote_type`: single/double |
| `cite` | `[@key]` | `key` |
| `note` | `[^note]` | - |

## Reader: read_markdown_blocks()

```sql
read_markdown_blocks(
    path VARCHAR,                    -- File path or glob pattern
    include_filepath := false,       -- Add file_path column (alias: filename)
    include_raw_html := false        -- Include raw HTML blocks
)
```

### Output Schema

Returns rows with duck_block shape:

| Column | Type | Description |
|--------|------|-------------|
| `kind` | VARCHAR | Always 'block' for this reader |
| `element_type` | VARCHAR | Block type identifier |
| `content` | VARCHAR | Block content |
| `level` | INTEGER | Document nesting depth (1 for top-level, 0 for frontmatter) |
| `encoding` | VARCHAR | 'text', 'json', or 'yaml' |
| `attributes` | MAP(VARCHAR, VARCHAR) | Block metadata (heading_level, language, id, etc.) |
| `element_order` | INTEGER | Position in document (1-indexed) |
| `file_path` | VARCHAR | Source file (when enabled) |

**Note on heading levels:** For headings, the actual H1-H6 level is stored in `attributes['heading_level']`, while `level` indicates document nesting depth (always 1 for top-level blocks). This matches the duck_block_utils convention.

### Examples

```sql
-- Basic usage
SELECT * FROM read_markdown_blocks('README.md');

-- Filter to headings with heading level
SELECT content, attributes['heading_level'] as heading_level
FROM read_markdown_blocks('doc.md')
WHERE element_type = 'heading'
ORDER BY element_order;

-- Extract code blocks by language
SELECT content, attributes['language'] as lang
FROM read_markdown_blocks('tutorial.md')
WHERE element_type = 'code';

-- Multi-file analysis
SELECT file_path, element_type, count(*) as count
FROM read_markdown_blocks('docs/**/*.md', include_filepath := true)
GROUP BY file_path, element_type;
```

## Writer: COPY TO Blocks

```sql
COPY query TO 'output.md' (
    FORMAT MARKDOWN,
    markdown_mode 'blocks',  -- or 'duck_block'
    -- Column mapping (defaults shown)
    kind_column 'kind',                -- 'block' or 'inline' (default: 'kind')
    element_type_column 'element_type',
    content_column 'content',
    level_column 'level',
    encoding_column 'encoding',
    attributes_column 'attributes'
);
```

The `kind` column determines how elements are rendered:
- `'block'` - Element is rendered with trailing newlines (paragraphs, headings, code blocks, etc.)
- `'inline'` - Element is rendered without trailing newlines (bold, italic, links, etc.)

When transitioning from inline to block elements, a paragraph break (`\n\n`) is automatically inserted.

### Rendering Rules

| element_type | Markdown Output |
|--------------|-----------------|
| `heading` | `#` × heading_level + space + content (uses `attributes['heading_level']`, falls back to `level`) |
| `paragraph` | content + blank line |
| `code` | ` ``` ` + language + newline + content + ` ``` ` |
| `blockquote` | `>` prefix per line |
| `list` | `- ` or `N. ` per item |
| `list_item` | `- ` for unordered, `N. ` for ordered (uses `attributes['ordered']` and `attributes['item_number']`) |
| `table` | `\| cell \|` format with separator |
| `hr` | `---` |
| `metadata` | `---` + content + `---` |
| `image` | `![alt](src "title")` |

### Examples

```sql
-- Round-trip a document
COPY (
    SELECT kind, element_type, content, level, encoding, attributes
    FROM read_markdown_blocks('input.md')
    ORDER BY element_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- Transform headings
COPY (
    SELECT
        kind,
        element_type,
        CASE WHEN element_type = 'heading'
             THEN upper(content)
             ELSE content END as content,
        level, encoding, attributes
    FROM read_markdown_blocks('doc.md')
    ORDER BY element_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');

-- Generate from scratch
COPY (
    SELECT * FROM (VALUES
        ('block', 'metadata', 'title: Generated Doc', 0, 'yaml', MAP{}::MAP(VARCHAR, VARCHAR), 0),
        ('block', 'heading', 'Introduction', 1, 'text', MAP{'id': 'intro'}::MAP(VARCHAR, VARCHAR), 1),
        ('block', 'paragraph', 'Welcome to this guide.', NULL, 'text', MAP{}::MAP(VARCHAR, VARCHAR), 2),
        ('block', 'code', 'print("hello")', NULL, 'text', MAP{'language': 'python'}::MAP(VARCHAR, VARCHAR), 3)
    ) AS t(kind, element_type, content, level, encoding, attributes, element_order)
) TO 'generated.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

## Duck Block Conversion Functions

Convert blocks back to Markdown without writing to files. These functions enable in-memory document transformation pipelines.

### duck_block_to_md(block)

Converts a single `duck_block` struct to a Markdown string.

```sql
SELECT duck_block_to_md({
    kind: 'block',
    element_type: 'heading',
    content: 'Hello World',
    level: 1,
    encoding: 'text',
    attributes: MAP{},
    element_order: 0
});
-- Returns: '# Hello World\n\n'
```

### duck_blocks_to_md(blocks[])

Converts a list of blocks to a complete Markdown document string.

```sql
-- Using the function
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('source.md') b;

-- Transform and render in one query
SELECT duck_blocks_to_md(
    list(b ORDER BY element_order)
)
FROM read_markdown_blocks('source.md') b;

-- Build document programmatically
SELECT duck_blocks_to_md([
    {kind: 'block', element_type: 'heading', content: 'Title', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'block', element_type: 'paragraph', content: 'Body.', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 1}
]);
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
        list(b ORDER BY element_order)
    )) as s
    FROM read_markdown_blocks('doc.md') b
);

-- Pipeline: read blocks -> filter -> convert to sections
SELECT s.*
FROM (
    SELECT unnest(duck_blocks_to_sections(
        list(b ORDER BY element_order)
    )) as s
    FROM read_markdown_blocks('tutorial.md') b
    WHERE b.element_type IN ('heading', 'paragraph', 'code')
);
```

## Inline Elements in Blocks

The `duck_block_to_md` and `duck_blocks_to_md` functions support both block and inline elements. Use `kind: 'inline'` for inline formatting:

```sql
-- Compose rich inline content
SELECT duck_blocks_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: 1, encoding: 'text', attributes: MAP{'href': 'https://docs.example.com'}, element_order: 1},
    {kind: 'inline', element_type: 'text', content: ' for ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 2},
    {kind: 'inline', element_type: 'bold', content: 'more info', level: 1, encoding: 'text', attributes: MAP{}, element_order: 3}
]);
-- Returns: 'Check out [our docs](https://docs.example.com) for **more info**'
```

### Using Inline Elements Within Block Content

Combine inline rendering with block functions for rich document generation:

```sql
-- Paragraph with formatted content
SELECT duck_block_to_md({
    kind: 'block',
    element_type: 'paragraph',
    content: duck_blocks_to_md([
        {kind: 'inline', element_type: 'text', content: 'Visit ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
        {kind: 'inline', element_type: 'link', content: 'GitHub', level: 1, encoding: 'text', attributes: MAP{'href': 'https://github.com'}, element_order: 1},
        {kind: 'inline', element_type: 'text', content: ' for projects.', level: 1, encoding: 'text', attributes: MAP{}, element_order: 2}
    ])::VARCHAR,
    level: NULL,
    encoding: 'text',
    attributes: MAP{},
    element_order: 0
});
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
| 2.1 | 2025-01 | Fixed level/heading_level: `level` is now document depth (1 for top-level), heading H1-H6 stored in `attributes['heading_level']`. Added `list_item` element type support. Fixed inline-to-block transitions. Added `filename` parameter alias. |
| 2.0 | 2025-01 | Unified on `duck_block` shape, removed `markdown_doc_block` type |
| 1.3 | 2025-01 | Added unified `doc_element` type with conversion functions |
| 1.2 | 2024-12 | Added duck_block conversion functions, `duck_block` COPY mode alias |
| 1.1 | 2024-12 | Aligned with doc_block_spec v1.0, added metadata type |
| 1.0 | 2024 | Initial implementation |
