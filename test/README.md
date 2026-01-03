# Testing the Markdown Extension

This directory contains the test suite for the DuckDB Markdown extension.

## Running Tests

```bash
# Run all tests
make test

# Run tests with debug build
make test_debug
```

## Test Structure

Tests are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html):

| File | Coverage |
|------|----------|
| `markdown.test` | Core markdown type and casting |
| `markdown_basic.test` | Basic file reading |
| `markdown_sections_*.test` | Section extraction and hierarchy |
| `markdown_blocks.test` | Block-level parsing |
| `markdown_copy.test` | COPY TO markdown modes (table, document, blocks) |
| `markdown_extraction_*.test` | Content extraction functions |
| `duck_block.test` | Duck block conversion functions |
| `doc_inline.test` | Inline element rendering |

## Test Data

- `test/markdown/` - Sample markdown files for reading tests
- `test/data/` - Block-level test fixtures
- `test/docs/` - Documentation-style test files

## Current Status

**949 assertions** across **18 test files** covering:
- File reading with glob patterns
- Section extraction with content modes
- Block parsing and round-trip
- Inline element composition
- COPY TO all three modes
- Cross-platform compatibility (Linux, macOS, Windows)
