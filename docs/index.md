# DuckDB Markdown Extension

This extension adds Markdown processing capabilities to DuckDB, enabling structured analysis of Markdown documents.

## Documentation

- [Duck Block Specification](doc_block_spec.md) - The duck_block structure for document representation
- [Markdown Implementation](markdown_doc_block.md) - Markdown-specific implementation details
- [Ecosystem Integration](ecosystem.md) - Related extensions and cross-format workflows

## Quick Start

```sql
-- Install and load the extension
INSTALL markdown FROM community;
LOAD markdown;

-- Read Markdown files
SELECT * FROM read_markdown('docs/**/*.md');

-- Parse markdown into blocks
SELECT * FROM read_markdown_blocks('README.md');

-- Extract sections with hierarchy
SELECT title, level, content
FROM read_markdown_sections('README.md', content_mode := 'full');
```

## Duck Block Functions

Convert blocks to/from Markdown:

```sql
-- Convert blocks back to markdown
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('doc.md') b;

-- Build inline content
SELECT duck_blocks_to_md([
    {kind: 'inline', element_type: 'text', content: 'Check out ', level: NULL, encoding: 'text', attributes: MAP{}, element_order: 0},
    {kind: 'inline', element_type: 'link', content: 'our docs', level: NULL, encoding: 'text', attributes: MAP{'href': 'https://example.com'}, element_order: 1}
]);
-- Returns: 'Check out [our docs](https://example.com)'

-- Convert blocks to sections
SELECT unnest(duck_blocks_to_sections(list(b ORDER BY element_order)))
FROM read_markdown_blocks('doc.md') b;
```

## COPY TO Markdown

Export query results in three modes:

```sql
-- Table mode (default): markdown table
COPY my_table TO 'output.md' (FORMAT MARKDOWN);

-- Document mode: reconstruct from sections
COPY sections TO 'doc.md' (FORMAT MARKDOWN, markdown_mode 'document');

-- Blocks mode: full-fidelity round-trip
COPY blocks TO 'copy.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

For complete documentation, see the [README](https://github.com/teaguesterling/duckdb_markdown).
