# test/markdown/simple.md
# Simple Document

This is a simple Markdown document for testing.

## Features

- Basic text formatting
- Lists
- Headers

---

# test/markdown/structured.md
# Introduction

This document demonstrates the hierarchical structure that the Markdown extension can parse.

## Getting Started

To get started with the extension, you need to install it first.

### Installation

```sql
INSTALL markdown FROM community;
LOAD markdown;
```

## Usage

Once installed, you can use the various functions:

```sql
SELECT * FROM read_markdown('docs/*.md');
```

---

# test/markdown/code_examples.md
# Code Examples

This document contains various code examples.

## SQL Examples

```sql
SELECT * FROM read_markdown_sections('docs/*.md');
```

## Python Examples

```python
import duckdb
conn = duckdb.connect()
conn.execute("LOAD markdown")
```

## JavaScript Examples

```javascript
const results = await db.query("SELECT * FROM 'README.md'");
```

---

# test/markdown/links.md
# Links Document

This document contains various types of links.

## External Links

Visit [DuckDB](https://duckdb.org) for more information.

## Internal Links

See the [Introduction](#introduction) section above.

## Reference Links

This is a [reference link][ref1] and another [reference][ref2].

[ref1]: https://example.com
[ref2]: https://example.org "Example Organization"

---

# test/markdown/metadata.md
---
title: Test Document
author: John Doe
date: 2024-01-15
tags: [test, markdown, documentation]
description: A test document with frontmatter metadata
---

# Test Document

This document has YAML frontmatter that should be extracted by the metadata functions.