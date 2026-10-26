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
- **Exception-free path.** `try_execute` is `noexcept` and reports failures
  through `std::expected<sqlite3_int64, std::string>`.
- **Precise errors.** Exceptions carry the result code, the extended code, the
  SQL text and the byte offset of the offending token
  (`sqlite3_error_offset`).
- **Snapshots and pragmas.** `serialize` / `deserialize` copy a whole database
  image in memory; `pragma` and `wal` cover the common connection settings.
- **C++23 throughout.** `std::string_view`, `std::optional`, `std::variant`,
  `std::span` BLOB binding, the `sqlite::bindable` concept, `[[nodiscard]]`
  result accessors and a hard `#error` below C++23.
- **Bundled SQLite.** `vendor/sqlite` carries the official SQLite 3.54.1
  amalgamation (SHA3-256 verified against sqlite.org); the test suite links it
  by default.

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

The full suite also runs clean under AddressSanitizer and
UndefinedBehaviorSanitizer in CI.

## Layout

| Path | Contents |
|---|---|
| `include/SQLiteCpp.h` | single include entry point |
| `include/SQLiteCpp/config.h` | version macros, C++23 requirement |
| `include/SQLiteCpp/types.h` | `str_ref`, `u16str_ref` |
| `include/SQLiteCpp/errors.h` | exception hierarchy with error offsets |
| `include/SQLiteCpp/fwd.h` | forward declarations, `bindable` concept, flags |
| `include/SQLiteCpp/binder.h` | stream binder, row iteration, tuple extraction |
| `include/SQLiteCpp/database.h` | connection, cache wiring, bulk and snapshot APIs |
| `include/SQLiteCpp/transaction.h` | RAII transactions |
| `include/SQLiteCpp/sql_function.h` | SQL function registration |
| `include/SQLiteCpp/detail/` | value traits, statement cache, UTF conversion |
| `benchmarks/` | micro-benchmarks |
| `vendor/sqlite/` | bundled SQLite 3.54.1 amalgamation |
| `scripts/` | ForgeDeps build validation |
| `tests/` | regression suite and installed-consumer project |

## Requirements

The headers require C++23. `vendor/sqlite` provides SQLite 3.54.1 for the test
suite; pass `-DSQLITECPP_BUNDLE_SQLITE=OFF` to link an external SQLite instead.
Installed consumers supply their own SQLite library.

## Build & Install

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/sdk
```

Installed consumers can use `find_package(SQLiteCpp CONFIG REQUIRED)` and link
`SQLiteCpp::SQLiteCpp` together with their SQLite target. ForgeRecipes can
supply both libraries through a ForgeDeps workspace.

## License

Released under the MIT license. See [LICENSE](LICENSE) for details.
