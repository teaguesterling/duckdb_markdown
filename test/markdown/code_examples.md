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