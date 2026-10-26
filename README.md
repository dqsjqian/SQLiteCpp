# SQLiteCpp

The most advanced modern C++23 wrapper for SQLite — stream-style parameter
binding, typed results, structured row mapping and a prepared-statement cache.

```cpp
#include <SQLiteCpp.h>

int main() {
    sqlite::database db(":memory:");
    db << "CREATE TABLE item (value INTEGER)";
    db << "INSERT INTO item VALUES (?)" << 42;
    int value = 0;
    db << "SELECT value FROM item" >> value;
    return value != 42;
}
```

## Highlights

- **Prepared statement cache.** Repeated SQL is compiled once
  (`sqlite3_prepare_v3` with `SQLITE_PREPARE_PERSISTENT`), then reset and
  reused. Capacity is set through `sqlite_config::statement_cache_size`
  (default 32, `0` disables reuse). A statement that is still executing is
  never handed out twice, so nested use of the same SQL stays correct.
- **RAII transactions.** `sqlite::transaction` rolls back on scope exit unless
  `commit()` was called; pick `deferred`, `immediate` or `exclusive`.
- **Bulk execution.** `execute_many` binds one tuple per range element and runs
  the same statement N times after a single compile.
- **Structured results.** Map columns onto a struct with member pointers, or
  read them into a tuple for structured bindings.
- **Exception-free path.** `try_execute` is `noexcept` and reports failures by
  value through `sqlite::execution_result` (changes, code, message).
- **Precise errors.** Exceptions carry the result code, the extended code, the
  SQL text and the byte offset of the offending token
  (`sqlite3_error_offset`).
- **Snapshots and pragmas.** `serialize` / `deserialize` copy a whole database
  image in memory; `pragma` and `wal` cover the common connection settings.
- **C++23 throughout.** `std::string_view`, `std::optional`, `std::variant`,
  `std::span` BLOB binding, the `sqlite::bindable` concept, `[[nodiscard]]`
  result accessors and a hard `#error` below C++23.
- **Concurrency and control.** `busy_timeout`, nested `savepoint`, `configure`
  for foreign keys / defensive mode, `interrupt` and a progress handler.
- **Diagnostics.** `explain` query plans, `cache_used` / `schema_used`,
  `read_header` decodes the 100-byte file header without a connection, and
  update / commit / rollback hooks plus `load_extension` are wired up.
- **Data channels.** `blob` streams a BLOB in chunks, `no_copy` binds with
  `SQLITE_STATIC`, and column metadata (name, declared type, origin table) is
  available on prepared statements.
- **SQL functions.** Scalar, aggregate and window functions, registered from
  plain lambdas.
- **Bundled SQLite, zero build dependencies.** `vendor/sqlite` carries the
  official SQLite 3.54.1 amalgamation (SHA3-256 verified against sqlite.org);
  CMake compiles it for the test suite by default. Clone, configure, build —
  no package manager, no downloaded toolchain, no network access required.

## API overview

| Area | Entry points |
|---|---|
| Connection | `database`, `sqlite_config`, `config_flag`, `busy_timeout`, `configure`, `interrupt`, `progress_handler`, `load_extension` |
| Statements | `operator<<`, `operator>>`, `execute`, `execute_many`, `try_execute`, `row.as<Ts...>()`, `column_name` / `column_declared_type` / `column_table_name` |
| Transactions | `transaction`, `savepoint` |
| Results | `select<Record>(...)`, `as<Ts...>()`, `pragma`, `explain` |
| Data | `blob` (incremental I/O), `no_copy` (SQLITE_STATIC), `serialize` / `deserialize` |
| Functions | `define` (scalar / aggregate / flags), `define_window` |
| Files | `read_header`, `has_sqlite_header` |
| Hooks | `on_update`, `on_commit`, `on_rollback` |
| Literals | `"SELECT 1"_sql` |
| Diagnostics | `cache_used`, `schema_used` |

## Bulk insert and transactions

```cpp
const std::vector<std::tuple<int, std::string>> rows{{1, "one"}, {2, "two"}};

sqlite::transaction tx(db, sqlite::transaction_behavior::immediate);
db.execute_many("INSERT INTO item VALUES (?, ?)", rows);
tx.commit();      // without commit() the transaction rolls back on scope exit
```

## Structured rows

```cpp
struct item_record {
    int id = 0;
    std::string value;
};

const auto items = db.select<item_record>(
    "SELECT id, value FROM item ORDER BY id",
    &item_record::id, &item_record::value);

for(auto &&row : db << "SELECT id, value FROM item") {
    auto [id, text] = row.as<int, std::string>();
}
```

## Performance

`tests/benchmark.cc`, Apple Silicon, `:memory:` database, 100k operations,
bundled SQLite 3.54.1:

| Operation | Per-statement prepare | With statement cache | Speed-up |
|---|---|---|---|
| 100k INSERT via `operator<<` | 227.6 ms (2.28 µs/op) | 125.7 ms (1.26 µs/op) | 1.81x |
| 100k SELECT via `operator>>` | 285.3 ms (2.85 µs/op) | 121.3 ms (1.21 µs/op) | 2.35x |
| 100k INSERT via `execute_many` | — | 61.3 ms (0.61 µs/op) | 3.71x |

Wrapper overhead against a hand-written C API loop, same workload, statement
reused in both cases, median of three runs:

| Path | Raw C API | SQLiteCpp | Overhead |
|---|---|---|---|
| 100k single-row INSERT, `db << sql` each row | 22.8 ms | 27.0 ms | +18% |
| 100k single-row SELECT, `db << sql` each row | 19.8 ms | 26.5 ms | +32% |
| 100k single-row INSERT, reused binder | 22.8 ms | 25.9 ms | +13% |
| 100k batched INSERT, `execute_many` | — | 22.8 ms | faster than the C loop |

Where the per-row cost goes: one binder object, one cache lookup, and a second
`step` that verifies a single-row result really is single. Holding the binder
(``auto stmt = db << "..."; stmt << ...; stmt.execute();``) removes the lookup
and lands at +13%. `execute_many` removes all of it and beats the C loop.

## Platforms

| Platform | Toolchains | Verified |
|---|---|---|
| Linux x86-64 | GCC 14, Clang 18 (libstdc++ and libc++), `-Werror` | build + tests |
| Linux ARM64 | GCC cross toolchain | cross-compile |
| macOS | AppleClang | build + tests |
| Windows | MSVC, ClangCL | build + tests |
| Windows MinGW | GCC UCRT64, `-Werror` | build + tests |
| iOS | AppleClang, iOS simulator arm64 | cross-compile |
| Android | NDK clang, arm64-v8a and armeabi-v7a | cross-compile |
| Quality | clang-tidy, lcov, libFuzzer, Doxygen | gates |

Every public header is also compiled on its own, and the suite runs under
AddressSanitizer and UndefinedBehaviorSanitizer.

## Build & Install

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/sdk
```

Installed consumers can use `find_package(SQLiteCpp CONFIG REQUIRED)` and link
`SQLiteCpp::SQLiteCpp` together with their SQLite target.

## License

Released under the MIT license. See [LICENSE](LICENSE) for details.
