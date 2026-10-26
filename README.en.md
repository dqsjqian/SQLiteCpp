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
  the same statement N times after a single compile — level with a
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

`benchmarks/benchmark.cc`, Release build, Apple Silicon, bundled SQLite 3.54.1,
100,000 operations. Every path does the same work: the row payloads are built
once before any timing, the C baseline keeps a prepared statement too, the
first run only warms caches and is discarded, the order of the paths rotates
between runs, and each figure is the median of nine runs.

`:memory:`:

| Path | Time | Per operation | vs. raw C API |
|---|---|---|---|
| **INSERT** (one transaction) | | | |
| Raw C API, one prepared statement reused | 20.3 ms | 203 ns | baseline |
| `db << sql << ...` per row, statement cache (default) | 23.6 ms | 236 ns | +16% |
| `db << sql << ...` per row, cache disabled | 69.3 ms | 693 ns | +242% (3.4x slower) |
| Held statement, rebound per row | 21.0 ms | 210 ns | +4% |
| `execute_many` | 20.5 ms | 205 ns | +1% (level) |
| **Single-row lookup by primary key** (TEXT result) | | | |
| Raw C API, one prepared statement reused | 23.0 ms | 230 ns | baseline |
| Raw C API, also stepping once more to `SQLITE_DONE` | 25.3 ms | 253 ns | +10% |
| `db << sql << id >> value` per row | 28.0 ms | 280 ns | +22% |
| Held statement, `query << id >> value` | 26.0 ms | 260 ns | +13% |
| **Full scan** (INTEGER + TEXT per row) | | | |
| Raw C API, step loop | 7.2 ms | 72 ns | baseline |
| `db << sql >> [](int, const std::string &)` | 7.3 ms | 73 ns | +2% |
| `for (auto &&row : db << sql) row.as<int, std::string>()` | 7.3 ms | 73 ns | +1% |
| `select<record>(...)` (the C side stores into a vector too) | 7.6 ms | 76 ns | level |

The same benchmark against a database file in WAL mode keeps the ratios:
per-row INSERT +13%, held statement +3%, `execute_many` level, cache disabled
+192%, single-row lookup +7% (five of those points are the single-row check
itself), scans +1% to +2%.

How to read it:

- **The per-row expression syntax costs about 33 ns**: one statement object,
  one cache lookup (a single comparison on a hit) and one connection lock.
  That is exactly what `execute_many` and a held statement remove.
- **Ten of the +22% on a single-row `>>` are semantics, not overhead**: it
  steps once more to prove the result really is a single row and otherwise
  throws `more_rows` at once. A C loop doing the same check pays the same ten
  points. The remaining twelve are the cache lookup and the statement object.
- **The cache-disabled row is a counter-example, not advice**: every row
  recompiles its SQL, and 693 ns is almost entirely compile cost; the cache
  takes that to 236 ns (1/2.9). Statements the cache does not keep are
  compiled without `SQLITE_PREPARE_PERSISTENT` — that hint is for statements
  kept and reused for a long time, and with a one-shot statement it only makes
  SQLite allocate outside the connection's lookaside memory. To save memory
  set the capacity to 1 to 4, not 0.
- **Scans cost next to nothing**: one SQLite call per column value (SQL NULL
  already reads as 0 or empty, so the type is not queried separately); three
  columns end up within 1 to 4 ns per row of the hand-written C loop.
- **`execute_many` is level with a held statement, not faster than C**: the
  1.1.0 claim of "8% faster" came from an unfair benchmark — `execute_many` got
  pre-built row data while the C baseline built its strings inside the timed
  region. Measured the same way, they are equal; compile once and loop is the
  ceiling the C API sets.

Against 1.1.1 on the same machine and the same benchmark: per-row INSERT +24%
→ +16%, single-row lookup +36% → +22%, full scan +29% → +1%, cache disabled
+557% → +242%.

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
