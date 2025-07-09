# DuckDB Markdown Extension - Future Development

## COPY TO Markdown Implementation

### Overview
Implement `COPY table TO 'file.md' (FORMAT MARKDOWN)` functionality that goes beyond simple table formatting to handle markdown-typed data intelligently.

### Implementation Approach
Based on DuckDB CLI's `ModeMarkdownRenderer` pattern from `tools/shell/shell_renderer.cpp`:
- Column separators: `" | "`
- Row start: `"| "`, row end: `" |\n"`
- Use `PrintMarkdownSeparator` for header separators with right-alignment for numeric columns
- Handle column width calculation and proper alignment

### Key Design Challenges

#### 1. Column Type Detection
The COPY function needs to intelligently handle different column types:

**Regular Data Columns** → Standard markdown table format:
```markdown
| Name | Age | Score |
|------|----:|------:|
| John |  25 |  95.5 |
```

**MARKDOWN-typed Columns** → Preserve original markdown content:
```markdown
# Combined Documentation

## From docs/readme.md
# My Project
This is the readme content...

---

## From docs/guide.md  
## Getting Started
This is the guide content...
```

#### 2. Section Reconstruction
When working with `read_markdown_sections()` output, the COPY function should:

**Scenario A: Document Reconstruction**
```sql
COPY (
  SELECT title, level, content 
  FROM read_markdown_sections('doc.md') 
  ORDER BY start_line
) TO 'reconstructed.md' (FORMAT MARKDOWN);
```
→ Rebuild original document hierarchy using `level` to generate `#`, `##`, etc.

**Scenario B: Multi-file Combination**
```sql
COPY (
  SELECT file_path, title, level, content 
  FROM read_markdown_sections('docs/*.md')
  WHERE title ILIKE '%installation%'
) TO 'installation-guide.md' (FORMAT MARKDOWN);
```
→ Create new document combining sections from multiple sources

**Scenario C: Content Restructuring**
```sql
COPY (
  SELECT 'API Reference' as title, 2 as level, content
  FROM read_markdown_sections('**/*.md') 
  WHERE title ILIKE '%api%'
) TO 'api-docs.md' (FORMAT MARKDOWN);
```
→ Generate new hierarchy and structure

#### 3. Output Format Options

**Option 1: Pure Table Format**
- Current CLI `.mode markdown` approach
- Standard markdown tables only

**Option 2: Enhanced Document Format**
```markdown
---
title: "Query Results"
date: 2025-01-09
query: "SELECT * FROM table"
rows: 42
generated_by: "DuckDB Markdown Extension"
---

# Query Results

| Column | Value |
|--------|-------|
| data1  | data2 |
```

**Option 3: Smart Mixed Format**
- Detect MARKDOWN columns and render as sections
- Regular columns as tables
- Optional frontmatter metadata
- Document structure preservation

### Required Functionality

#### Column Analysis
1. **Type Detection**: Identify MARKDOWN vs regular columns
2. **Special Column Recognition**: 
   - `title` + `level` → Generate headings
   - `file_path` → Section organization
   - `content` (MARKDOWN) → Raw markdown output

#### Rendering Logic
1. **Heading Generation**: Convert `level` integers to markdown heading syntax
2. **Content Preservation**: Output MARKDOWN columns without escaping
3. **Table Formatting**: Standard markdown tables for regular data
4. **Metadata Injection**: Optional frontmatter generation

#### Configuration Options
- `header := true/false` - Include frontmatter metadata
- `format := 'table'|'document'|'mixed'` - Output format style  
- `separator := '---'` - Section separator for multi-document output
- `title := 'Custom Title'` - Override document title

### Use Cases

#### Documentation Workflows
```sql
-- Extract all API documentation
COPY (
  SELECT title, level, content 
  FROM read_markdown_sections('**/*.md')
  WHERE title ILIKE '%api%'
  ORDER BY file_path, start_line
) TO 'api-reference.md' (FORMAT MARKDOWN, format := 'document');

-- Create installation guide from multiple sources
COPY (
  SELECT file_path, title, content
  FROM read_markdown_sections('docs/*.md')
  WHERE title ILIKE '%install%' OR title ILIKE '%setup%'
) TO 'installation.md' (FORMAT MARKDOWN, format := 'mixed');

-- Export data analysis results with context
COPY (
  SELECT 
    'Analysis Results' as title,
    1 as level,
    '## Summary\n\nProcessed ' || count(*) || ' records.' as content
  FROM my_table
  UNION ALL
  SELECT 'Data Table', 2, NULL
  -- Regular table data would follow
) TO 'report.md' (FORMAT MARKDOWN);
```

#### Round-trip Scenarios
```sql
-- Read → Process → Write workflow
COPY (
  SELECT 
    'Combined Documentation' as title,
    1 as level,
    content
  FROM read_markdown('docs/*.md')
  WHERE md_stats(content).word_count > 100
) TO 'filtered-docs.md' (FORMAT MARKDOWN);
```

### Implementation Priority
1. **Phase 1**: Basic table format (replicating CLI `.mode markdown`)
2. **Phase 2**: MARKDOWN column detection and raw output
3. **Phase 3**: Section reconstruction (title/level handling)
4. **Phase 4**: Enhanced document formatting with frontmatter
5. **Phase 5**: Advanced configuration options

### Technical Notes
- Leverage existing `ModeMarkdownRenderer` patterns from DuckDB CLI
- Ensure proper escaping for table content vs raw markdown output
- Handle edge cases: empty content, malformed hierarchies, conflicting column names
- Consider streaming implementation for large document processing
- Maintain compatibility with existing `read_markdown` and `read_markdown_sections` output formats

This feature would make the markdown extension a powerful tool for **documentation engineering** - enabling SQL-driven content management, documentation generation, and knowledge base construction workflows.