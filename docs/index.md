# DuckDB Markdown Extension

This extension adds Markdown processing capabilities to DuckDB, enabling structured analysis of Markdown documents.

## Documentation

- [Document Block Specification](doc_block_spec.md) - Format-agnostic specification for representing documents as blocks
- [Markdown Implementation](markdown_doc_block.md) - Markdown-specific implementation details
- [Ecosystem Integration](ecosystem.md) - Using with webbed (HTML/XML) and duck_block_utils
- [Updating](UPDATING.md) - Guide for updating the extension

## Quick Start

```sql
-- Install and load the extension
INSTALL markdown FROM community;
LOAD markdown;

-- Read Markdown files
SELECT * FROM read_markdown('docs/**/*.md');

-- Parse markdown into blocks
SELECT * FROM read_markdown_blocks('README.md');
```

## Duck Block Functions

Convert blocks back to Markdown without writing to files:

```sql
-- Convert a single block to markdown
SELECT duck_block_to_md(block) FROM read_markdown_blocks('doc.md');

-- Convert a list of blocks to a complete document
SELECT duck_blocks_to_md(list(b ORDER BY block_order))
FROM read_markdown_blocks('doc.md') b;

-- Convert blocks to hierarchical sections
SELECT unnest(duck_blocks_to_sections(list(b ORDER BY block_order)))
FROM read_markdown_blocks('doc.md') b;
```

See [Markdown Implementation](markdown_doc_block.md#duck-block-conversion-functions) for details.

## Inline Element Functions

Build rich text content with structured inline elements:

```sql
-- Convert inline elements to markdown
SELECT doc_inlines_to_md([
    {inline_type: 'text', content: 'Check out ', attributes: MAP{}},
    {inline_type: 'link', content: 'our docs', attributes: MAP{'href': 'https://example.com'}},
    {inline_type: 'text', content: ' for ', attributes: MAP{}},
    {inline_type: 'bold', content: 'more info', attributes: MAP{}}
]::doc_inline[]);
-- Returns: 'Check out [our docs](https://example.com) for **more info**'
```

Supported types: `link`, `image`, `bold`, `italic`, `code`, `text`, `strikethrough`, `linebreak`

See [Inline Element Functions](markdown_doc_block.md#inline-element-functions) for details.

For full usage details, see the [main README](https://github.com/teaguesterling/duckdb_markdown).
