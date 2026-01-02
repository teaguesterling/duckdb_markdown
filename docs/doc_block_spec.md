# Document Block Specification

**Version**: 1.0
**Status**: Draft
**Last Updated**: 2024-12

A format-agnostic specification for representing documents as sequences of typed blocks in DuckDB. This specification enables interoperability between document format extensions, allowing documents to be converted, transformed, and analyzed using SQL.

> **Note**: The duckdb_markdown extension uses the `duck_block` shape from `duck_block_utils`, which extends this specification with:
> - `kind` column ('block' or 'inline') for unified block/inline representation
> - `element_type` column (instead of `block_type`)
> - `element_order` column (instead of `block_order`)
>
> See [Markdown Implementation](markdown_doc_block.md) for details.

## Overview

Documents are represented as ordered sequences of **blocks**. Each block is a row with standardized columns, enabling:

- **Format conversion**: Read HTML → transform → write Markdown
- **Cross-format queries**: Analyze structure across document types
- **Unified tooling**: Common utilities work with any compliant extension
- **SQL-based transformation**: Filter, aggregate, and manipulate documents

### Design Principles

1. **Minimal core**: Small set of universal block types that map across formats
2. **Extensible**: Formats can define additional block types with namespaced identifiers
3. **Lossless where possible**: Preserve source structure; note where normalization occurs
4. **SQL-native**: Use DuckDB types naturally (VARCHAR, INTEGER, MAP, LIST)

## Core Schema

All compliant extensions MUST produce rows with these columns:

| Column | Type | Required | Description |
|--------|------|----------|-------------|
| `block_type` | VARCHAR | Yes | Block type identifier |
| `content` | VARCHAR | Yes | Primary content of the block |
| `level` | INTEGER | No | Hierarchy level, nesting depth, or NULL |
| `encoding` | VARCHAR | Yes | Content encoding (see below) |
| `attributes` | MAP(VARCHAR, VARCHAR) | Yes | Type-specific metadata |
| `block_order` | INTEGER | Yes | Sequential position in document (0-indexed) |
| `source_format` | VARCHAR | No | Origin format identifier (e.g., 'markdown', 'html') |
| `file_path` | VARCHAR | No | Source file path |

### Column Semantics

#### block_type
Identifies the block's semantic type. Uses either:
- **Core type**: One of the universal types defined below
- **Namespaced type**: Format-specific type as `format:type` (e.g., `html:div`, `yaml:anchor`)

#### content
The primary textual content of the block. Interpretation depends on `encoding`:
- For `text`: Raw text content, may include inline formatting
- For `json`: Valid JSON string
- For `yaml`: Valid YAML string
- For `html`: HTML fragment
- For `xml`: XML fragment

#### level
Semantic level or nesting depth:
- Headings: 1-6 (or format equivalent)
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
Key-value metadata specific to the block type. All values are VARCHAR (convert as needed).

Common attributes:
| Attribute | Description | Used By |
|-----------|-------------|---------|
| `id` | Element identifier | headings, anchors |
| `language` | Code language | code blocks |
| `ordered` | List ordering ('true'/'false') | lists |
| `start` | List start number | ordered lists |
| `src` | Source URL | images, embeds |
| `alt` | Alternative text | images |
| `title` | Title text | links, images |
| `class` | CSS class(es) | HTML elements |

#### source_format
Optional identifier of the originating format. Enables tracking provenance through conversions:
- `markdown`, `html`, `xml`, `yaml`, `rst`, `latex`, `pandoc`, etc.

## Core Block Types

These block types MUST be recognized by all compliant extensions. Extensions SHOULD map format-specific elements to these types where semantically appropriate.

| Type | Description | Content Encoding | Level | Key Attributes |
|------|-------------|------------------|-------|----------------|
| `heading` | Section heading | text | 1-6 | `id` |
| `paragraph` | Text paragraph | text | NULL | - |
| `code` | Code block | text | NULL | `language` |
| `blockquote` | Quoted content | text | depth (1+) | - |
| `list` | List of items | json | 1 | `ordered`, `start` |
| `table` | Tabular data | json | NULL | - |
| `hr` | Thematic break | text (empty) | NULL | - |
| `metadata` | Document metadata | yaml or json | 0 | - |
| `image` | Image reference | text (empty) | NULL | `src`, `alt`, `title` |
| `raw` | Raw format-specific content | varies | NULL | `format` |

### Block Type Details

#### heading
Section or document headings.

```
block_type: 'heading'
content: 'Introduction'
level: 1
encoding: 'text'
attributes: {'id': 'introduction'}
```

#### paragraph
Block of text. Inline formatting (bold, italic, links) is preserved in content.

```
block_type: 'paragraph'
content: 'This is a paragraph with **bold** and *italic* text.'
level: NULL
encoding: 'text'
attributes: {}
```

#### code
Preformatted code or monospace text.

```
block_type: 'code'
content: 'def hello():\n    print("Hello")'
level: NULL
encoding: 'text'
attributes: {'language': 'python'}
```

#### blockquote
Quoted or indented content. Level indicates nesting depth.

```
block_type: 'blockquote'
content: 'To be or not to be.'
level: 1
encoding: 'text'
attributes: {}
```

#### list
Ordered or unordered list. Content is JSON array.

**Simple list:**
```
block_type: 'list'
content: '["Item 1", "Item 2", "Item 3"]'
level: 1
encoding: 'json'
attributes: {'ordered': 'false'}
```

**Nested list:**
```
block_type: 'list'
content: '[{"text": "Parent", "children": ["Child 1", "Child 2"]}, "Sibling"]'
level: 1
encoding: 'json'
attributes: {'ordered': 'false'}
```

#### table
Tabular data as JSON with headers and rows.

```
block_type: 'table'
content: '{"headers": ["Name", "Age"], "rows": [["Alice", "30"], ["Bob", "25"]]}'
level: NULL
encoding: 'json'
attributes: {}
```

Optional alignment in attributes:
```
attributes: {'align': '["left", "right"]'}
```

#### hr
Thematic break / horizontal rule.

```
block_type: 'hr'
content: ''
level: NULL
encoding: 'text'
attributes: {}
```

#### metadata
Document metadata (frontmatter, head elements, etc.). Level 0 indicates document-level.

```
block_type: 'metadata'
content: 'title: My Document\nauthor: Jane Doe'
level: 0
encoding: 'yaml'
attributes: {}
```

#### image
Image reference. Content is empty; details in attributes.

```
block_type: 'image'
content: ''
level: NULL
encoding: 'text'
attributes: {'src': 'photo.jpg', 'alt': 'A photo', 'title': 'My Photo'}
```

#### raw
Preserved format-specific content that doesn't map to core types.

```
block_type: 'raw'
content: '<custom-element>Content</custom-element>'
level: NULL
encoding: 'html'
attributes: {'format': 'html'}
```

## Format-Specific Extensions

Extensions MAY define additional block types using namespaced identifiers: `format:type`

### Namespace Registry

| Namespace | Format | Extension |
|-----------|--------|-----------|
| `md` | Markdown | duckdb_markdown |
| `html` | HTML | duckdb_webbed |
| `xml` | XML | duckdb_webbed |
| `yaml` | YAML | duckdb_yaml |
| `pandoc` | Pandoc AST | panduck |

### Extension Examples

**Markdown-specific:**
- `md:footnote` - Footnote definition
- `md:definition_list` - Definition list
- `md:task_list` - Task/checkbox list

**HTML-specific:**
- `html:div` - Generic div container
- `html:script` - Script element
- `html:style` - Style element
- `html:form` - Form element

**XML-specific:**
- `xml:element` - Generic XML element
- `xml:cdata` - CDATA section
- `xml:processing_instruction` - Processing instruction

**YAML-specific:**
- `yaml:document` - YAML document boundary
- `yaml:anchor` - Named anchor
- `yaml:alias` - Anchor reference

## Conversion Guidelines

When converting between formats, extensions SHOULD:

1. **Map to core types** where semantically equivalent
2. **Preserve as `raw`** when no equivalent exists
3. **Set `source_format`** to track provenance
4. **Document lossy conversions** in extension docs

### Conversion Matrix (Informative)

| Source → Target | Guidance |
|-----------------|----------|
| HTML → Markdown | `<h1>` → `heading`, `<p>` → `paragraph`, `<pre><code>` → `code` |
| Markdown → HTML | `#` → `heading`, inline formatting expanded |
| YAML → Blocks | Top-level keys as `metadata`, nested structures as appropriate |
| Pandoc → Blocks | Map Pandoc AST types to nearest core equivalent |

## SQL Type Registration

Extensions MAY register a named STRUCT type for programmatic construction:

```sql
-- Generic doc_block type
CREATE TYPE doc_block AS STRUCT(
    block_type VARCHAR,
    content VARCHAR,
    level INTEGER,
    encoding VARCHAR,
    attributes MAP(VARCHAR, VARCHAR),
    block_order INTEGER
);

-- Format-specific aliases
CREATE TYPE markdown_doc_block AS doc_block;
CREATE TYPE html_doc_block AS doc_block;
```

## Writer Requirements

Extensions providing COPY TO functionality MUST:

1. Accept rows matching the core schema
2. Handle all core block types (or error gracefully)
3. Support custom column name mapping via options
4. Document format-specific rendering rules

### Standard COPY TO Options

| Option | Type | Description |
|--------|------|-------------|
| `block_type_column` | VARCHAR | Column containing block type |
| `content_column` | VARCHAR | Column containing content |
| `level_column` | VARCHAR | Column containing level |
| `encoding_column` | VARCHAR | Column containing encoding |
| `attributes_column` | VARCHAR | Column containing attributes |

## Utility Functions

The `doc_block_utils` extension provides format-agnostic helpers:

### Proposed Functions

```sql
-- Block manipulation
doc_blocks_filter(blocks, types[])        -- Filter to specific types
doc_blocks_transform(blocks, mapping)     -- Apply transformations
doc_blocks_merge(blocks1, blocks2)        -- Merge block sequences
doc_blocks_reorder(blocks)                -- Renumber block_order

-- Content extraction
doc_blocks_to_text(blocks)                -- Extract plain text
doc_blocks_headings(blocks)               -- Extract heading hierarchy
doc_blocks_toc(blocks)                    -- Generate table of contents

-- Validation
doc_blocks_validate(blocks)               -- Check schema compliance
doc_blocks_lint(blocks)                   -- Check for common issues

-- Conversion helpers
doc_blocks_set_source(blocks, format)     -- Set source_format
doc_blocks_normalize(blocks)              -- Normalize to core types only
```

## Versioning

This specification uses semantic versioning:
- **Major**: Breaking schema changes
- **Minor**: New optional columns or block types
- **Patch**: Clarifications and documentation

Extensions SHOULD declare which spec version they target.

## Appendix A: JSON Schemas

### List Content Schema
```json
{
  "oneOf": [
    {"type": "array", "items": {"type": "string"}},
    {"type": "array", "items": {
      "type": "object",
      "properties": {
        "text": {"type": "string"},
        "children": {"$ref": "#"}
      }
    }}
  ]
}
```

### Table Content Schema
```json
{
  "type": "object",
  "required": ["headers", "rows"],
  "properties": {
    "headers": {"type": "array", "items": {"type": "string"}},
    "rows": {"type": "array", "items": {
      "type": "array", "items": {"type": "string"}
    }}
  }
}
```

## Appendix B: Reference Implementation

The DuckDB Markdown extension (`duckdb_markdown`) serves as the reference implementation:

- Repository: https://github.com/teaguesterling/duckdb_markdown
- Reader: `read_markdown_blocks()`
- Writer: `COPY TO ... (FORMAT MARKDOWN, markdown_mode 'blocks')`

## Appendix C: Ecosystem Extensions

| Extension | Purpose | Status |
|-----------|---------|--------|
| `duckdb_markdown` | Markdown reading/writing | Released |
| `duckdb_webbed` | HTML/XML parsing | Planned |
| `duckdb_yaml` | YAML document parsing | Planned |
| `doc_block_utils` | Cross-format utilities | Planned |
| `panduck` | Pandoc AST integration | Planned |
