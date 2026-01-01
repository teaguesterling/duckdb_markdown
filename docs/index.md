# DuckDB Markdown Extension

This extension adds Markdown processing capabilities to DuckDB, enabling structured analysis of Markdown documents.

## Documentation

- [Document Block Specification](doc_block_spec.md) - Format-agnostic specification for representing documents as blocks
- [Markdown Implementation](markdown_doc_block.md) - Markdown-specific implementation details
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

For full usage details, see the [main README](https://github.com/teaguesterling/duckdb_markdown).
