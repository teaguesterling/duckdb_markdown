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
| `duck_block_roundtrip.test` | Round-trip: blocks → markdown → blocks |
| `docs_roundtrip.test` | Round-trip test using actual docs/*.md files |

## Test Data

- `test/markdown/` - Sample markdown files for reading tests
- `test/data/` - Block-level test fixtures
- `test/docs/` - Documentation-style test files
- `docs/` - Actual extension documentation (used by docs_roundtrip.test)

## Current Status

**1102 assertions** across **20 test files** covering:
- File reading with glob patterns
- Section extraction with content modes
- Block parsing and round-trip
- Inline element composition
- COPY TO all three modes
- Cross-platform compatibility (Linux, macOS, Windows)
- level/heading_level attribute handling
- Nested document structures (up to H4 depth)
- Real-world docs round-trip validation
