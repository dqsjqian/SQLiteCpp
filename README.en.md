# SQLiteCpp

[简体中文](README.md) | English

The most advanced modern C++23 wrapper for SQLite — stream-style parameter
binding, typed results, structured row mapping and a prepared-statement cache.
Header-only, self-contained, one SQLite version.

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
  (default 32, `0` disables caching). A statement that is still executing is
  never handed out twice, so nested use of the same SQL stays correct.
- **Thread-safe sharing.** The cache locks the connection's own SQLite mutex,
  so one `sqlite::database` can be shared by several threads exactly as SQLite
  allows (serialized mode). Verified under ThreadSanitizer.
- **Predictable binding.** Every execution starts from a clean slate:
  parameters you do not bind read as `NULL`, even on a cached statement, and a
  held statement can be rebound and re-executed as often as you like.
- **RAII transactions.** `sqlite::transaction` rolls back on scope exit unless
  `commit()` was called; pick `deferred`, `immediate` or `exclusive`. Nested
  `sqlite::savepoint` works the same way.
- **Bulk execution.** `execute_many` binds one tuple per range element and runs
  the same statement N times after a single compile — faster than a
  hand-written C loop.
- **Structured results.** Map columns onto a struct with member pointers, or
  read them into a tuple for structured bindings.
- **Exception-free path.** `try_execute` is `noexcept` and reports failures by
  value through `sqlite::execution_result` (changes, code, message).
- **Precise errors.** Exceptions carry the result code, the extended code, the
  SQL text and the byte offset of the offending token
  (`sqlite3_error_offset`).
- **Concurrency and control.** `busy_timeout`, `configure` for foreign keys /
  defensive mode, `interrupt` and a progress handler.
- **Diagnostics.** `explain` query plans, `cache_used` / `schema_used`,
  `read_header` decodes the 100-byte file header without a connection, plus
  update / commit / rollback hooks and `load_extension`.
- **Data channels.** `blob` streams a BLOB in chunks, `no_copy` binds with
  `SQLITE_STATIC`, `serialize` / `deserialize` copy a whole database image,
  and column metadata (name, declared type, origin table) is available on
  every statement. All lengths go through the 64-bit SQLite APIs.
- **SQL functions.** Scalar, aggregate and window functions, registered from
  plain lambdas.
- **C++23 throughout.** `std::string_view`, `std::optional`, `std::variant`,
  `std::span`, the `sqlite::bindable` concept, `[[nodiscard]]` accessors and a
  hard `#error` below C++23.
- **Bundled SQLite, zero build dependencies.** `vendor/sqlite` carries the
  official SQLite 3.54.1 amalgamation (SHA3-256 verified against sqlite.org).
  Clone, configure, build — no package manager, no downloaded toolchain, no
  network access required.

## API overview

| Area | Entry points |
|---|---|
| Connection | `database`, `sqlite_config`, `config_flag`, `busy_timeout`, `configure`, `interrupt`, `progress_handler`, `load_extension` |
| Statements | `operator<<`, `operator>>`, `execute`, `execute_many`, `try_execute`, `row.as<Ts...>()`, `column_name` / `column_declared_type` / `column_table_name` |
| Transactions | `transaction`, `savepoint` |
| Results | `select<Record>(...)`, `as<Ts...>()`, `pragma`, `explain` |
| Data | `blob` (incremental I/O), `no_copy` (`SQLITE_STATIC`), `serialize` / `deserialize`, `backup` |
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

## Reusing a statement

```cpp
auto insert = db << "INSERT INTO item VALUES (?, ?)";
for(const auto &[id, text] : rows) {
    insert << id << text;
    insert.execute();      // resets the statement; the next << starts at ?1
}
```

Parameters a round does not bind are `NULL`, never a value left over from an
earlier execution of the same cached SQL.

## Threads

A `sqlite::database` may be shared between threads when SQLite runs in
serialized mode — SQLite's default, and the mode of the bundled amalgamation.
The statement cache takes the connection's own mutex, so concurrent
`db << ...` calls are safe. A single statement object (the value returned by
`db << sql`) belongs to one thread at a time, and hooks or progress handlers
should be installed before the connection is shared. Connections opened with
`OpenFlags::NOMUTEX` have no mutex: use them from one thread at a time, and the
cache pays nothing for locking.

## Hooks

```cpp
db.on_update([](int action, std::string_view database, std::string_view table,
                sqlite3_int64 rowid) {
    // database / table point into SQLite memory: copy them to keep them
});
db.on_update(nullptr);     // removes the hook
```

Handlers taking `std::string` names keep working. Exceptions thrown by a hook
never unwind through SQLite: a failing progress handler interrupts the
statement, and a failing commit hook turns the commit into a rollback.

## Performance

`benchmarks/benchmark.cc`, Release build, Apple Silicon, `:memory:`, bundled
SQLite 3.54.1, 100,000 operations, median of seven runs:

| Path | Time | Per operation | vs. raw C API |
|---|---|---|---|
| Raw C API, one prepared INSERT reused | 23.3 ms | 0.23 µs | baseline |
| `db << sql << ...` per row, cache disabled | 131.9 ms | 1.32 µs | 5.7x slower |
| `db << sql << ...` per row, statement cache (default) | 27.8 ms | 0.28 µs | +19% |
| Held statement, rebound per row | 23.7 ms | 0.24 µs | +2% |
| `execute_many` | 21.4 ms | 0.21 µs | 8% faster |
| Raw C API, one prepared SELECT reused | 20.3 ms | 0.20 µs | baseline |
| `db << sql << id >> value` per row | 27.9 ms | 0.28 µs | +37% |

The statement cache is worth 4.7x on per-row SQL. What remains of the per-row
gap is the expression syntax itself: one statement object and one cache lookup
per row, and for `>>` into a single value a second `step` that proves the
result really is a single row. Holding the statement removes the lookup;
`execute_many` removes everything and beats the C loop.

## Platforms

Every job builds with warnings as errors
(`-Wall -Wextra -Wpedantic -Werror`, `/W4 /WX /permissive-` for MSVC and
clang-cl) and runs on GitHub's `-latest` runner images. Every compiler is the
newest stable release rather than whatever the image happens to preinstall:
GCC 16 from the official `gcc:16` image, LLVM/Clang 23 from the LLVM project's
apt repository on Linux and its release package on Windows.

| Platform | Runner | Toolchain | Verified |
|---|---|---|---|
| Linux x86-64 | `ubuntu-latest` | GCC 16; Clang 23 (libstdc++ and libc++), Debug and Release | build, tests, install |
| Linux ARM64 | `ubuntu-26.04-arm` | GCC 16, native | build, tests |
| macOS | `macos-latest` | AppleClang (newest released Xcode on the image) | build, tests, install |
| iOS | `macos-latest` | AppleClang (same), simulator arm64 | build |
| Android | `ubuntu-latest` | NDK r30 (newest on the image), arm64-v8a and armeabi-v7a | build |
| Windows x64 | `windows-latest` | MSVC (Visual Studio 2026); clang-cl 23 | build, tests |
| Windows ARM64 | `windows-11-vs2026-arm` | MSVC (Visual Studio 2026), native | build, tests |
| Windows MinGW | `windows-latest` | MSYS2 UCRT64 GCC (tracks MSYS2, currently 16.2) | build, tests |

Quality gates on every push: AddressSanitizer + UndefinedBehaviorSanitizer
(GCC 16), ThreadSanitizer (Clang 23), clang-tidy 23 (defect checks, findings
are errors), lcov coverage of `include/`, a 60-second libFuzzer run over the
SQL front end, Doxygen API docs, and every public header compiled on its own.

## Requirements

The headers require C++23 and SQLite 3.38 or newer. The repository vendors the
official SQLite 3.54.1 amalgamation in `vendor/sqlite` and the test suite always
builds that copy. An installed package ships the same amalgamation;
`SQLiteCpp_SQLITE_DIR` points at it. A consumer that links its own SQLite must
use 3.38 or newer; older versions are rejected at compile time with a clear
message.

## Build & Install

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix /path/to/sdk
```

```cmake
project(app LANGUAGES C CXX)             # C is needed for sqlite3.c
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(sqlite3_core STATIC ${SQLiteCpp_SQLITE_DIR}/sqlite3.c)
target_include_directories(sqlite3_core PUBLIC ${SQLiteCpp_SQLITE_DIR})
target_link_libraries(sqlite3_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

target_link_libraries(app PRIVATE SQLiteCpp::SQLiteCpp sqlite3_core)
```

| CMake option | Default | Effect |
|---|---|---|
| `BUILD_TESTING` | `ON` (CTest) | regression suite and benchmark |
| `SQLITECPP_WARNINGS_AS_ERRORS` | `OFF` | strict warnings as errors for tests and tools |
| `SQLITECPP_COVERAGE` | `OFF` | `--coverage` instrumentation (GCC / Clang) |
| `SQLITECPP_BUILD_FUZZERS` | `OFF` | libFuzzer target (LLVM Clang) |

## Layout

| Path | Contents |
|---|---|
| `include/SQLiteCpp.h` | single include entry point |
| `include/SQLiteCpp/config.h` | version macros, C++23 requirement |
| `include/SQLiteCpp/types.h` | string reference types |
| `include/SQLiteCpp/errors.h` | exception hierarchy |
| `include/SQLiteCpp/fwd.h` | forward declarations, flags, `execution_result` |
| `include/SQLiteCpp/binder.h` | statements, binding, row iteration |
| `include/SQLiteCpp/database.h` | connection, bulk / snapshot / pragma / hook APIs |
| `include/SQLiteCpp/transaction.h` | `transaction`, `savepoint` |
| `include/SQLiteCpp/sql_function.h` | scalar, aggregate and window functions |
| `include/SQLiteCpp/backup.h` | online backup between connections |
| `include/SQLiteCpp/blob.h` | incremental BLOB I/O |
| `include/SQLiteCpp/diagnostics.h` | database file header decoding |
| `include/SQLiteCpp/literals.h` | `_sql` literals |
| `include/SQLiteCpp/detail/` | value traits, statement cache, UTF conversion |
| `vendor/sqlite/` | bundled SQLite 3.54.1 amalgamation |
| `tests/` | regression suite, thread test, installed-consumer project |
| `benchmarks/` | micro-benchmarks against the raw C API |
| `fuzz/` | libFuzzer target |
| `docs/` | Doxygen configuration |
| `.github/` | CI workflow and toolchain setup scripts |

## License

Released under the MIT license. See [LICENSE](LICENSE) for details.
