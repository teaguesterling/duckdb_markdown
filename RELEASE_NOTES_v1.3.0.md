# Release Notes: v1.3.0

## Overview

This release introduces significant new features for document block manipulation, including inline element support and a unified `duck_block` type system. **This is a breaking release** that changes the schema for `read_markdown_blocks()` and COPY TO blocks mode.

## Breaking Changes

### Schema Changes for `read_markdown_blocks()`

The output schema has been updated to use the `duck_block` shape from the `duck_block_utils` specification:

| Old Column | New Column | Notes |
|------------|------------|-------|
| - | `kind` | New column, always `'block'` |
| `block_type` | `element_type` | Renamed |
| `block_order` | `element_order` | Renamed |

**Migration example:**
```sql
-- v1.2.0
SELECT block_type, content, block_order
FROM read_markdown_blocks('doc.md');

-- v1.3.0
SELECT element_type, content, element_order
FROM read_markdown_blocks('doc.md');
```

### COPY TO Blocks Mode

- Option `block_type_column` renamed to `element_type_column`
- Input queries should use `element_type` instead of `block_type`

### Removed Features

- Removed `markdown_doc_block` type registration
- Removed `doc_element_to_md()` and `doc_elements_to_md()` functions (replaced by `duck_block_to_md()` and `duck_blocks_to_md()`)

## New Features

### Unified Block and Inline Element Support

The `duck_block_to_md()` and `duck_blocks_to_md()` functions now support both block and inline elements via the `kind` field:

```sql
-- Block elements (kind = 'block')
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

-- Inline elements (kind = 'inline')
SELECT duck_blocks_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: 1, encoding: 'text', attributes: MAP{'href': 'https://example.com'}, element_order: 1},
    {kind: 'inline', element_type: 'bold', content: '!', level: 1, encoding: 'text', attributes: MAP{}, element_order: 2}
]);
-- Returns: 'Check out [our docs](https://example.com) **!**'
```

### Supported Inline Element Types

| Type | Output | Attributes |
|------|--------|------------|
| `link` | `[text](href "title")` | `href`, `title` |
| `image` | `![alt](src "title")` | `src`, `title` |
| `bold` / `strong` | `**text**` | - |
| `italic` / `em` | `*text*` | - |
| `code` | `` `text` `` | - |
| `strikethrough` / `del` | `~~text~~` | - |
| `superscript` / `sup` | `^text^` | - |
| `subscript` / `sub` | `~text~` | - |
| `math` | `$text$` or `$$text$$` | `display` |
| `smallcaps` | `<span style="...">` | - |
| `text`, `space`, `linebreak` | Various | - |

### `heading_level` Attribute Support

Headings now support the `heading_level` attribute which takes priority over the `level` field:

```sql
SELECT duck_block_to_md({
    kind: 'block',
    element_type: 'heading',
    content: 'Title',
    level: 1,  -- Ignored when heading_level is set
    encoding: 'text',
    attributes: MAP{'heading_level': '3'},
    element_order: 0
});
-- Returns: '### Title\n\n'
```

### Duck Block Conversion Functions

New functions for converting blocks to markdown without file I/O:

- `duck_block_to_md(block)` - Convert single block to markdown
- `duck_blocks_to_md(blocks[])` - Convert list of blocks to markdown document
- `duck_blocks_to_sections(blocks[])` - Convert blocks to hierarchical sections

## Bug Fixes

- Fixed image alt text fallback in `duck_blocks_to_md` when `alt` attribute is missing
- Fixed table rendering in COPY TO blocks mode
- Fixed list rendering for ordered/unordered lists
- Fixed NULL title handling in link/image extraction
- Fixed `md_extract_tables_json` struct type mismatch
- Implemented `md_section_breadcrumb` function (was placeholder)
- Implemented reference link detection in `md_extract_links`

## Documentation

- Added comprehensive ecosystem integration documentation
- Added Document Block Specification (format-agnostic)
- Switched documentation to MkDocs on ReadTheDocs
- Added inline element examples to README

## Improvements

- Added comprehensive edge case tests for blocks functionality
- Added tests for unicode content, error handling, and round-trip stability
- Improved documentation throughout

## Upgrade Guide

1. **Update column references**: Change `block_type` → `element_type` and `block_order` → `element_order` in your queries

2. **Update COPY TO options**: Change `block_type_column` → `element_type_column`

3. **Update function calls**: Replace `doc_element_to_md` → `duck_block_to_md` and `doc_elements_to_md` → `duck_blocks_to_md`

4. **Add kind column if constructing blocks manually**: Include `kind: 'block'` for block elements
