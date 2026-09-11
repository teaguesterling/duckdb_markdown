# Ecosystem Integration

The DuckDB Markdown extension is part of a planned document processing ecosystem. This page describes related extensions and cross-format workflows.

## Extension Status

| Extension | Purpose | Status |
|-----------|---------|--------|
| [duckdb_markdown](https://github.com/teaguesterling/duckdb_markdown) | Markdown processing | **Released** |
| [duckdb_webbed](https://github.com/teaguesterling/duckdb_webbed) | HTML/XML processing | *Planned* |
| [duckdb_duck_block_utils](https://github.com/teaguesterling/duckdb_duck_block_utils) | Block manipulation utilities | *Planned* |

## Current Capabilities (duckdb_markdown)

The markdown extension provides complete block-level document processing:

```sql
LOAD markdown;

-- Read markdown into duck_block rows
SELECT * FROM read_markdown_blocks('README.md');

-- Convert blocks back to markdown
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('doc.md') b;

-- Convert blocks to hierarchical sections
SELECT unnest(duck_blocks_to_sections(list(b ORDER BY element_order)))
FROM read_markdown_blocks('doc.md') b;
```

### Block-Level Transformations

```sql
-- Filter and transform blocks
SELECT duck_blocks_to_md(list(b ORDER BY element_order))
FROM read_markdown_blocks('doc.md') b
WHERE element_type IN ('heading', 'paragraph', 'code');

-- Extract code blocks with language
SELECT content, attributes['language'] as lang
FROM read_markdown_blocks('tutorial.md')
WHERE element_type = 'code';

-- Round-trip with modifications
COPY (
    SELECT kind, element_type,
           CASE WHEN element_type = 'heading' THEN upper(content) ELSE content END as content,
           level, encoding, attributes
    FROM read_markdown_blocks('input.md')
    ORDER BY element_order
) TO 'output.md' (FORMAT MARKDOWN, markdown_mode 'blocks');
```

## Planned: Cross-Format Conversion

When `webbed` and `duck_block_utils` are available, cross-format workflows will be possible:

### Markdown to HTML (Planned)

```sql
-- Future: Convert markdown blocks to HTML
LOAD markdown;
LOAD webbed;

SELECT duck_blocks_to_html(
    list(b ORDER BY element_order)
)
FROM read_markdown_blocks('README.md') b;
```

### HTML to Markdown (Planned)

```sql
-- Future: Convert HTML to markdown via duck_block
LOAD markdown;
LOAD webbed;

SELECT duck_blocks_to_md(
    html_to_duck_blocks('<h1>Title</h1><p>Content</p>')
);
```

## Planned: Block Utilities

The `duck_block_utils` extension will provide format-agnostic block manipulation:

### Consuming markdown's blocks from duck_block_utils

These are no longer planned — `duck_blocks_filter`, `_exclude`, `_to_text`,
`_toc`, `_validate` and `_stats` all ship in `duck_block_utils` today, and
markdown's blocks feed them directly.

**This document does not reproduce their signatures.** They live in another
repository, on its own release cadence, and a copy here goes stale silently:
internal spec 6.5 (retired; the line was later renumbered, and public 1.2
supersedes internal 6.6) reshaped four extractors so that `duck_blocks_headings`, `_links`,
`_code_blocks` and `_toc` return **duck_blocks** rather than bespoke projection
structs, with the previous shapes preserved as `_structs` siblings
(`duck_blocks_toc_structs` and so on). An example pinned here would have been
wrong the day that landed, and this one was.

For the current surface, read `duck_block_utils`' own documentation — it is the
authority, and `make check-vocabulary` in this repo verifies only the vendored
*vocabulary constants*, not the function signatures.

```sql
LOAD markdown;
LOAD duck_block_utils;

-- markdown produces blocks; duck_block_utils consumes them. Whatever the
-- consuming function's current return shape is, this is the handoff:
SELECT duck_blocks_to_text(
    (SELECT list(b ORDER BY element_order) FROM read_markdown_blocks('README.md') b)
);
```

## The duck_block Specification

All ecosystem extensions share the common `duck_block` structure:

```sql
STRUCT(
    kind          VARCHAR,              -- 'block', 'inline' or 'value' (metadata)
    element_type  VARCHAR,              -- 'heading', 'paragraph', 'bold', etc.
    content       VARCHAR,              -- Text content
    level         INTEGER,              -- Heading level or nesting depth
    encoding      VARCHAR,              -- 'text', 'json', 'yaml'
    attributes    MAP(VARCHAR, VARCHAR),-- Element metadata
    element_order INTEGER               -- Position in sequence
)
```

This shared structure enables:

- **Format conversion**: Read one format, write another
- **Cross-format queries**: Analyze structure across document types
- **Unified tooling**: Common utilities work with any format
- **SQL-based transformation**: Filter, aggregate, and manipulate documents

See [Duck Block Specification](doc_block_spec.md) for complete details.
