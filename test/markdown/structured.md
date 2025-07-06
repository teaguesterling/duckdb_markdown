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