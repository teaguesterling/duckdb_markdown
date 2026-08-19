# Broken Encoding Fixture

This file deliberately contains invalid UTF-8: a lone Latin-1 byte (é)
and a UTF-16 BOM pair (ÿþ) in the middle of the text.

| a | b |
|---|---|
| xÃ | y |
