# DuckDB Markdown Extension

This extension adds Markdown processing capabilities to DuckDB, enabling structured analysis of Markdown documents.

## Documentation

- [Document Block Specification](doc_block_spec.md) - Format-agnostic specification for representing documents as blocks
- [Markdown Implementation](markdown_doc_block.md) - Markdown-specific implementation details
- [Ecosystem Integration](ecosystem.md) - Using with webbed (HTML/XML) and duck_block_utils

## Quick Start

```sql
-- Install and load the extension
INSTALL markdown FROM community;
LOAD markdown;

-- Read Markdown files
SELECT * FROM read_markdown('docs/**/*.md');

-- Parse markdown into blocks (duck_block shape)
SELECT * FROM read_markdown_blocks('README.md');
```

## Duck Block Functions

Convert blocks back to Markdown without writing to files:

```sql
-- Convert a single block to markdown
SELECT duck_block_to_md(block) FROM read_markdown_blocks('doc.md');

-- Convert a list of blocks to a complete document
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('doc.md') b;

-- Convert blocks to hierarchical sections
SELECT unnest(duck_blocks_to_sections(list(b ORDER BY element_order)))
FROM read_markdown_blocks('doc.md') b;
```

See [Markdown Implementation](markdown_doc_block.md#duck-block-conversion-functions) for details.

## Inline Elements

Build rich text content with the unified `duck_block` type that supports both block and inline elements:

```sql
-- Convert inline elements to markdown
SELECT duck_blocks_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: 1, encoding: 'text', attributes: MAP{'href': 'https://example.com'}, element_order: 1},
    {kind: 'inline', element_type: 'text', content: ' for ', level: 1, encoding: 'text', attributes: MAP{}, element_order: 2},
    {kind: 'inline', element_type: 'bold', content: 'more info', level: 1, encoding: 'text', attributes: MAP{}, element_order: 3}
]);
-- Returns: 'Check out [our docs](https://example.com) for **more info**'
```

Supported inline types: `link`, `image`, `bold`, `italic`, `code`, `text`, `strikethrough`, `linebreak`, `math`, `superscript`, `subscript`

See [Inline Elements in Blocks](markdown_doc_block.md#inline-elements-in-blocks) for details.

For full usage details, see the [main README](https://github.com/teaguesterling/duckdb_markdown).
