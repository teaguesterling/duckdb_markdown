# Duck Block Specification

**Version**: 2.0
**Status**: Stable
**Last Updated**: 2025-01

A format-agnostic specification for representing documents as sequences of typed blocks in DuckDB. This specification enables interoperability between document format extensions, allowing documents to be converted, transformed, and analyzed using SQL.

## Overview

Documents are represented as ordered sequences of **blocks**. Each block is a row with standardized columns, enabling:

- **Format conversion**: Read HTML → transform → write Markdown
- **Cross-format queries**: Analyze structure across document types
- **Unified tooling**: Common utilities work with any compliant extension
- **SQL-based transformation**: Filter, aggregate, and manipulate documents

### Design Principles

1. **Minimal core**: Small set of universal element types that map across formats
2. **Extensible**: Formats can define additional element types with namespaced identifiers
3. **Lossless where possible**: Preserve source structure; note where normalization occurs
4. **SQL-native**: Use DuckDB types naturally (VARCHAR, INTEGER, MAP, LIST)

## Core Schema (duck_block shape)

All compliant extensions MUST produce rows with these columns:

| Column | Type | Required | Description |
|--------|------|----------|-------------|
| `kind` | VARCHAR | Yes | 'block' or 'inline' |
| `element_type` | VARCHAR | Yes | Element type identifier |
| `content` | VARCHAR | Yes | Primary content of the element |
| `level` | INTEGER | No | Hierarchy level, nesting depth, or NULL |
| `encoding` | VARCHAR | Yes | Content encoding (see below) |
| `attributes` | MAP(VARCHAR, VARCHAR) | Yes | Type-specific metadata |
| `element_order` | INTEGER | Yes | Sequential position in document (0-indexed) |

Optional columns:
| Column | Type | Description |
|--------|------|-------------|
| `file_path` | VARCHAR | Source file path (when reading multiple files) |

### Column Semantics

#### kind
Distinguishes between block-level and inline elements:
- **'block'**: Block-level elements (headings, paragraphs, code blocks, lists, tables)
- **'inline'**: Inline elements (bold, italic, links, images within text)

Block elements are rendered with trailing newlines. Inline elements are concatenated without trailing newlines.

#### element_type
Identifies the element's semantic type. Uses either:
- **Core type**: One of the universal types defined below
- **Namespaced type**: Format-specific type as `format:type` (e.g., `html:div`, `md:footnote`)

#### content
The primary textual content of the element. Interpretation depends on `encoding`:
- For `text`: Raw text content, may include inline formatting
- For `json`: Valid JSON string
- For `yaml`: Valid YAML string
- For `html`: HTML fragment
- For `xml`: XML fragment

#### level
Semantic level or nesting depth:
- Headings: 1-6 (or use `attributes['heading_level']` for priority)
- Nested structures: Depth from root (1+)
- Frontmatter/metadata: 0
- Not applicable: NULL

#### encoding
Declares how `content` should be interpreted:

| Encoding | Description | Use Cases |
|----------|-------------|-----------|
| `text` | Plain text, possibly with inline markup | Paragraphs, headings, code |
| `json` | JSON-encoded structured data | Lists, tables, complex structures |
| `yaml` | YAML-encoded structured data | Frontmatter, metadata |
| `html` | HTML fragment | Preserved HTML in markdown |
| `xml` | XML fragment | Preserved XML content |

#### attributes
Key-value metadata specific to the element type. All values are VARCHAR (convert as needed).

Common attributes:
| Attribute | Description | Used By |
|-----------|-------------|---------|
| `id` | Element identifier | headings, anchors |
| `heading_level` | Heading level (takes priority over `level` field) | headings |
| `language` | Code language | code blocks |
| `ordered` | List ordering ('true'/'false') | lists |
| `start` | List start number | ordered lists |
| `src` | Source URL | images, embeds |
| `href` | Link URL | links |
| `alt` | Alternative text | images |
| `title` | Title text | links, images |

## Core Block Types (kind = 'block')

These element types MUST be recognized by all compliant extensions:

| Type | Description | Content Encoding | Level | Key Attributes |
|------|-------------|------------------|-------|----------------|
| `heading` | Section heading | text | 1-6 | `id`, `heading_level` |
| `paragraph` | Text paragraph | text | NULL | - |
| `code` | Code block | text | NULL | `language` |
| `blockquote` | Quoted content | text | depth (1+) | - |
| `list` | List of items | json | 1 | `ordered`, `start` |
| `table` | Tabular data | json | NULL | - |
| `hr` | Thematic break | text (empty) | NULL | - |
| `metadata` | Document metadata | yaml or json | 0 | - |
| `frontmatter` | Document metadata | yaml | 0 | - |
| `image` | Block-level image | text (empty) | NULL | `src`, `alt`, `title` |
| `raw` | Raw format-specific content | varies | NULL | `format` |

### Block Type Details

#### heading
Section or document headings. Level can be specified via `attributes['heading_level']` (preferred) or the `level` field.

```
kind: 'block'
element_type: 'heading'
content: 'Introduction'
level: 1
encoding: 'text'
attributes: {'id': 'introduction', 'heading_level': '1'}
```

#### paragraph
Block of text. Inline formatting (bold, italic, links) is preserved in content.

```
kind: 'block'
element_type: 'paragraph'
content: 'This is a paragraph with **bold** and *italic* text.'
level: NULL
encoding: 'text'
attributes: {}
```

#### code
Preformatted code or monospace text.

```
kind: 'block'
element_type: 'code'
content: 'def hello():\n    print("Hello")'
level: NULL
encoding: 'text'
attributes: {'language': 'python'}
```

#### list
Ordered or unordered list. Content is JSON array.

```
kind: 'block'
element_type: 'list'
content: '["Item 1", "Item 2", "Item 3"]'
level: 1
encoding: 'json'
attributes: {'ordered': 'false'}
```

#### table
Tabular data as JSON with headers and rows.

```
kind: 'block'
element_type: 'table'
content: '{"headers": ["Name", "Age"], "rows": [["Alice", "30"], ["Bob", "25"]]}'
level: NULL
encoding: 'json'
attributes: {}
```

#### metadata / frontmatter
Document metadata (frontmatter, head elements, etc.). Level 0 indicates document-level.

```
kind: 'block'
element_type: 'frontmatter'
content: 'title: My Document\nauthor: Jane Doe'
level: 0
encoding: 'yaml'
attributes: {}
```

## Core Inline Types (kind = 'inline')

Inline elements for rich text composition:

| Type | Description | Output | Key Attributes |
|------|-------------|--------|----------------|
| `text` | Plain text | as-is | - |
| `bold` / `strong` | Bold text | `**text**` | - |
| `italic` / `em` | Italic text | `*text*` | - |
| `code` | Inline code | `` `text` `` | - |
| `link` | Hyperlink | `[text](href)` | `href`, `title` |
| `image` | Inline image | `![alt](src)` | `src`, `title` |
| `strikethrough` / `del` | Strikethrough | `~~text~~` | - |
| `superscript` / `sup` | Superscript | `^text^` | - |
| `subscript` / `sub` | Subscript | `~text~` | - |
| `linebreak` / `br` | Hard line break | `<br>` | - |
| `math` | Math expression | `$text$` | `display`: inline/block |

### Inline Element Example

```sql
-- Compose rich text from inline elements
SELECT duck_blocks_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: NULL, encoding: 'text', attributes: MAP{'href': 'https://example.com'}, element_order: 1},
    {kind: 'inline', element_type: 'text', content: ' for ', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 2},
    {kind: 'inline', element_type: 'bold', content: 'more info', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 3}
]);
-- Returns: 'Check out [our docs](https://example.com) for **more info**'
```

## Format-Specific Extensions

Extensions MAY define additional element types using namespaced identifiers: `format:type`

### Namespace Registry

| Namespace | Format | Extension |
|-----------|--------|-----------|
| `md` | Markdown | duckdb_markdown |
| `html` | HTML | duckdb_webbed |
| `xml` | XML | duckdb_webbed |

### Extension Examples

**Markdown-specific:**
- `md:footnote` - Footnote definition
- `md:task_list` - Task/checkbox list
- `md:html_block` - Raw HTML in markdown

**HTML-specific:**
- `html:div` - Generic div container
- `html:script` - Script element

## Writer Requirements

Extensions providing COPY TO functionality MUST:

1. Accept rows matching the core schema
2. Handle all core element types (or error gracefully)
3. Support custom column name mapping via options
4. Handle block/inline transitions appropriately

### Standard COPY TO Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `kind_column` | VARCHAR | 'kind' | Column containing kind |
| `element_type_column` | VARCHAR | 'element_type' | Column containing element type |
| `content_column` | VARCHAR | 'content' | Column containing content |
| `level_column` | VARCHAR | 'level' | Column containing level |
| `encoding_column` | VARCHAR | 'encoding' | Column containing encoding |
| `attributes_column` | VARCHAR | 'attributes' | Column containing attributes |

## SQL Usage Examples

### Reading and Writing

```sql
-- Read markdown into duck_block rows
SELECT * FROM read_markdown_blocks('doc.md');

-- Round-trip: read, transform, write
COPY (
    SELECT kind, element_type, upper(content) as content, level, encoding, attributes
    FROM read_markdown_blocks('input.md')
    WHERE element_type != 'hr'
    ORDER BY element_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

### Converting Blocks to Markdown

```sql
-- Convert blocks back to markdown string
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('doc.md') b;

-- Build document programmatically
SELECT duck_blocks_to_md([
    {kind: 'block', element_type: 'heading', content: 'Title', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'block', element_type: 'paragraph', content: 'Body text.', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 1}
]);
```

### Converting Blocks to Sections

```sql
-- Get hierarchical sections from blocks
SELECT s.section_id, s.level, s.title
FROM (
    SELECT unnest(duck_blocks_to_sections(list(b ORDER BY element_order))) as s
    FROM read_markdown_blocks('doc.md') b
);
```

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 2.0 | 2025-01 | Added `kind` column, renamed to `element_type`/`element_order`, added inline support |
| 1.0 | 2024-12 | Initial specification with `block_type`/`block_order` |

## Reference Implementation

The DuckDB Markdown extension (`duckdb_markdown`) serves as the reference implementation:

- Repository: https://github.com/teaguesterling/duckdb_markdown
- Documentation: https://duckdb-markdown.readthedocs.io/
- Reader: `read_markdown_blocks()`
- Writer: `COPY TO ... (FORMAT MARKDOWN, markdown_mode 'blocks')`
- Converters: `duck_block_to_md()`, `duck_blocks_to_md()`, `duck_blocks_to_sections()`
