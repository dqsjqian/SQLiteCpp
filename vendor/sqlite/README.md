# Bundled SQLite

| | |
|---|---|
| Version | 3.54.1 (`SQLITE_VERSION_NUMBER` 3054001) |
| Source | https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip |
| SHA3-256 | `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30` |
| License | public domain, https://www.sqlite.org/copyright.html |

`sqlite3.c`, `sqlite3.h` and `sqlite3ext.h` are the unmodified upstream
amalgamation.

## Updating

1. Download the new amalgamation zip from https://www.sqlite.org/download.html.
2. Verify its SHA3-256 against the published value.
3. Replace the three files above and update this table.
4. Update the version assertion in `tests/maintenance_regression.cpp`.
