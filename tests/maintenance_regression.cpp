#include <SQLiteCpp.h>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

static void require(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(expression);
}
#define CHECK(expression) require((expression), #expression)

static void run_suite() {
    sqlite::database db(":memory:");
    db << "CREATE TABLE item (id INTEGER PRIMARY KEY, value TEXT, missing INTEGER, data BLOB)";
    const std::string_view text = "hello";
    const std::vector<unsigned char> data{0, 1, 255};
    db << "INSERT INTO item VALUES (?, ?, ?, ?)" << 1 << text << std::optional<int>{} << data;
    std::string found;
    std::optional<int> absent;
    std::vector<unsigned char> blob;
    db << "SELECT value, missing, data FROM item WHERE id=:id" << sqlite::named_parameter(":id", 1)
       >> [&](const std::string &value, std::optional<int> missing, const std::vector<unsigned char> &bytes) {
            found = value; absent = missing; blob = bytes;
       };
    CHECK(found == text && !absent && blob == data);
    auto reusable = db << "SELECT ?";
    int result = 0;
    reusable << 41 >> result;
    CHECK(result == 41);
    reusable << 42 >> result;
    CHECK(result == 42);
    db << "SELECT ?2 + ?1" << sqlite::indexed_parameter(2, 20) << sqlite::indexed_parameter(1, 22) >> result;
    CHECK(result == 42);
    std::u16string unicode;
    db << u"SELECT ?" << std::u16string(u"hello \u4e16\u754c") >> unicode;
    CHECK(unicode == u"hello \u4e16\u754c");
    db.define("echo_utf16", [](std::u16string value) { return value; });
    const std::u16string embedded(u"a\0b\U0001F600", 5);
    db << "SELECT echo_utf16(?)" << embedded >> unicode;
    CHECK(unicode == embedded);
    bool failed = false;
    try { db << "SELECT missing_column FROM item" >> result; }
    catch (const sqlite::sqlite_exception&) { failed = true; }
    CHECK(failed);
    db << "SELECT COUNT(*) FROM item" >> result;
    CHECK(result == 1);

    // std::span binds a borrowed BLOB view; read it back into an owning vector
    const std::vector<unsigned char> payload{9, 8, 7, 6};
    db << "INSERT INTO item VALUES (?, NULL, NULL, ?)" << 2
       << std::span<const unsigned char>(payload);
    std::vector<unsigned char> roundtrip;
    db << "SELECT data FROM item WHERE id=2" >> roundtrip;
    CHECK(roundtrip == payload);

    // empty containers bind a zero-length BLOB, not SQL NULL
    db << "INSERT INTO item VALUES (?, NULL, NULL, ?)" << 3 << std::vector<unsigned char>{};
    int is_null = -1;
    db << "SELECT data IS NULL FROM item WHERE id=3" >> is_null;
    CHECK(is_null == 0);

#ifdef SQLITECPP_BUNDLED_SQLITE
    CHECK(sqlite3_libversion_number() == 3054001);
#endif

    // execute_many: one prepare, N executions
    db << "CREATE TABLE batch (id INTEGER, label TEXT)";
    const std::vector<std::tuple<int, std::string>> batch_rows{
        {1, "one"}, {2, "two"}, {3, "three"}};
    const auto inserted = db.execute_many("INSERT INTO batch VALUES (?, ?)", batch_rows);
    CHECK(inserted == 3);
    db << "SELECT COUNT(*) FROM batch" >> result;
    CHECK(result == 3);

    // transaction: rolls back on scope exit unless committed
    {
        sqlite::transaction tx(db);
        db << "INSERT INTO batch VALUES (99, 'rolled-back')";
    }
    db << "SELECT COUNT(*) FROM batch" >> result;
    CHECK(result == 3);

    {
        sqlite::transaction tx(db, sqlite::transaction_behavior::immediate);
        db << "INSERT INTO batch VALUES (100, 'committed')";
        tx.commit();
    }
    db << "SELECT COUNT(*) FROM batch" >> result;
    CHECK(result == 4);

    // try_execute: value-based error reporting without exceptions
    const auto ok = db.try_execute("SELECT ?", 7);
    CHECK(ok.has_value());
    const auto bad = db.try_execute("SELECT * FROM no_such_table");
    CHECK(!bad.has_value() && !bad.error.empty() && bad.code != SQLITE_OK);

    // syntax errors carry a byte offset into the SQL text
    bool offset_seen = false;
    try {
        db << "SELECT FROM" >> result;
    } catch(const sqlite::sqlite_exception &error) {
        offset_seen = error.get_error_offset() >= 0;
    }
    CHECK(offset_seen);

    // public concept gates what may be bound
    static_assert(sqlite::bindable<int>);
    static_assert(sqlite::bindable<std::string_view>);
    static_assert(sqlite::bindable<std::vector<unsigned char>>);
    static_assert(!sqlite::bindable<void>);

    // structured row access: columns assigned to member pointers in order
    struct item_record {
        int id = 0;
        std::string value;
    };
    const auto records = db.select<item_record>(
        "SELECT id, value FROM item WHERE data IS NOT NULL ORDER BY id",
        &item_record::id, &item_record::value);
    CHECK(records.size() == 3 && records[0].id == 1 && records[0].value == "hello");

    // as<Ts...>() enables structured bindings directly
    {
        auto single = db << "SELECT id, value FROM item WHERE id = 1";
        auto row = single.begin();
        CHECK(row != single.end());
        auto [id, row_text] = (*row).as<int, std::string>();
        CHECK(id == 1 && row_text == "hello");
    }

    // pragma reads and the WAL helper
    CHECK(db.pragma("PRAGMA journal_mode") == "memory");
    // UTF-16 to UTF-8 across every code-point width
    CHECK(sqlite::utility::utf16_to_utf8(u"A\u00E9\u4E16\U0001F600")
          == std::string("A") + "\xC3\xA9" + "\xE4\xB8\x96" + "\xF0\x9F\x98\x80");

    // lone surrogates are rejected
    bool invalid_utf16 = false;
    try {
        sqlite::utility::utf16_to_utf8(std::u16string(1, char16_t(0xD800)));
    } catch(const sqlite::sqlite_exception &) {
        invalid_utf16 = true;
    }
    CHECK(invalid_utf16);
    CHECK(!db.wal()); // WAL is not available for :memory: databases

    // serialize / deserialize round-trip
    const auto image = db.serialize();
    CHECK(!image.empty());
    sqlite::database restored(":memory:");
    restored.deserialize(image);
    int restored_count = 0;
    restored << "SELECT COUNT(*) FROM item" >> restored_count;
    db << "SELECT COUNT(*) FROM item" >> result;
    CHECK(restored_count == result);
    // backup copies a database into another connection
    {
        sqlite::database source(":memory:");
        source << "CREATE TABLE copied (v TEXT)";
        source << "INSERT INTO copied VALUES (?)" << "payload";
        sqlite::database target(":memory:");
        sqlite::backup bk(source, target);
        bk.run();
        CHECK(bk.done());
        bk.finish();
        std::string copied;
        target << "SELECT v FROM copied" >> copied;
        CHECK(copied == "payload");
    }

    // progress handler runs during long statements and can abort them
    {
        int calls = 0;
        db.progress_handler([&calls] { ++calls; return 0; }, 50);
        db << "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 500)"
              " SELECT COUNT(*) FROM c" >> result;
        CHECK(result == 500);
        db.clear_progress_handler();
        CHECK(calls > 0);

        bool aborted = false;
        db.progress_handler([] { return 1; }, 1);
        try {
            db << "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 500)"
                  " SELECT COUNT(*) FROM c" >> result;
        } catch(const sqlite::errors::interrupt &) {
            aborted = true;
        }
        db.clear_progress_handler();
        CHECK(aborted);
    }
    // busy timeout is reflected by the connection
    db.busy_timeout(250);
    CHECK(db.pragma("PRAGMA busy_timeout") == "250");

    // db_config toggles, foreign keys enforced
    db.configure(sqlite::config_flag::foreign_keys, true);
    CHECK(db.configured(sqlite::config_flag::foreign_keys));
    db << "CREATE TABLE parent (id INTEGER PRIMARY KEY)";
    db << "CREATE TABLE child (pid INTEGER REFERENCES parent(id))";
    bool fk_violation = false;
    try {
        db.execute("INSERT INTO child VALUES (123)");
    } catch(const sqlite::sqlite_exception &) {
        fk_violation = true;
    }
    CHECK(fk_violation);
    db.configure(sqlite::config_flag::foreign_keys, false);

    // savepoints nest: an inner rollback keeps the outer work
    db << "CREATE TABLE sp (v INTEGER)";
    {
        sqlite::savepoint outer(db, "outer");
        db << "INSERT INTO sp VALUES (1)";
        {
            sqlite::savepoint inner(db, "inner");
            db << "INSERT INTO sp VALUES (2)";
            // leaves scope without commit: rolled back
        }
        db << "INSERT INTO sp VALUES (3)";
        outer.commit();
    }
    db << "SELECT COUNT(*) FROM sp" >> result;
    CHECK(result == 2);
    db << "SELECT SUM(v) FROM sp" >> result;
    CHECK(result == 4);
    // zero-copy binding keeps the caller's storage, no copy is made
    const std::string_view stable = "zero-copy";
    std::string zero_copy_text;
    db << "SELECT ?" << sqlite::no_copy(stable) >> zero_copy_text;
    CHECK(zero_copy_text == "zero-copy");

    const std::vector<unsigned char> zero_copy_bytes{1, 2, 3};
    std::vector<unsigned char> zero_copy_out;
    db << "SELECT ?" << sqlite::no_copy(zero_copy_bytes) >> zero_copy_out;
    CHECK(zero_copy_out == zero_copy_bytes);

    // column metadata
    db << "CREATE TABLE meta (id INTEGER, label TEXT)";
    auto meta = db << "SELECT id, label FROM meta";
    CHECK(meta.column_count() == 2);
    CHECK(meta.column_name(0) == "id");
    CHECK(meta.column_declared_type(0) == "INTEGER");
    CHECK(meta.column_declared_type(1) == "TEXT");
#ifdef SQLITE_ENABLE_COLUMN_METADATA
    CHECK(meta.column_table_name(1) == "meta");
    CHECK(meta.column_origin_name(1) == "label");
#endif
    meta.execute();
    // incremental BLOB I/O: read and patch a 1 KiB BLOB in chunks
    db << "CREATE TABLE blobs (id INTEGER PRIMARY KEY, data BLOB)";
    std::vector<unsigned char> large(1024);
    for(int i = 0; i < 1024; ++i) large[static_cast<std::size_t>(i)] = static_cast<unsigned char>(i % 251);
    db << "INSERT INTO blobs VALUES (1, ?)" << large;
    {
        sqlite::blob handle(db, "blobs", "data", 1);
        CHECK(handle.size() == 1024);
        std::vector<unsigned char> head(16, 0);
        handle.read(std::span<unsigned char>(head.data(), head.size()), 0);
        CHECK(std::equal(head.begin(), head.end(), large.begin()));
        const std::vector<unsigned char> patch{9, 9, 9};
        handle.write(std::span<const unsigned char>(patch.data(), patch.size()), 100);
    }
    std::vector<unsigned char> patched;
    db << "SELECT data FROM blobs WHERE id = 1" >> patched;
    CHECK(patched.size() == 1024 && patched[100] == 9 && patched[101] == 9 && patched[103] == large[103]);
    // hooks: update fires per row, commit and rollback per transaction
    {
        int updates = 0;
        int commits = 0;
        int rollbacks = 0;
        std::string updated_table;
        db.on_update([&](int, std::string_view, std::string_view table, sqlite3_int64) {
            ++updates;
            updated_table = std::string(table);
        });
        db.on_commit([&commits] { ++commits; return 0; });
        db.on_rollback([&rollbacks] { ++rollbacks; });
        db << "CREATE TABLE hooked (v INTEGER)";
        db.execute("INSERT INTO hooked VALUES (1)");
        CHECK(updates == 1 && updated_table == "hooked");
        {
            sqlite::transaction tx(db);
            db.execute("INSERT INTO hooked VALUES (2)");
            tx.commit();
        }
        CHECK(commits > 0);
        {
            sqlite::transaction tx(db);
            db.execute("INSERT INTO hooked VALUES (3)");
            // rolled back on scope exit
        }
        CHECK(rollbacks > 0);
        db.on_update(nullptr);
        db.on_commit({});
        db.on_rollback({});

        // 1.1.0-style handler taking std::string names still compiles and runs
        std::string legacy_table;
        // By-value std::string parameters are the point: this is the 1.1.0 signature.
        // NOLINTNEXTLINE(performance-unnecessary-value-param)
        db.on_update([&legacy_table](int, std::string, std::string table, sqlite3_int64) {
            legacy_table = std::move(table);
        });
        db.execute("INSERT INTO hooked VALUES (4)");
        db.on_update(nullptr);
        CHECK(legacy_table == "hooked");

        // an empty std::function or a null function pointer removes the hook;
        // a captureless lambda (convertible to a function pointer) installs it
        int stateless_calls = 0;
        static int *stateless_counter = nullptr;
        stateless_counter = &stateless_calls;
        db.on_update([](int, std::string_view, std::string_view, sqlite3_int64) { ++*stateless_counter; });
        db.execute("INSERT INTO hooked VALUES (5)");
        CHECK(stateless_calls == 1);
        db.on_update(std::function<void(int, std::string_view, std::string_view, sqlite3_int64)>{});
        void (*no_handler)(int, std::string_view, std::string_view, sqlite3_int64) = nullptr;
        db.on_update(no_handler);
        db.execute("INSERT INTO hooked VALUES (6)");
        CHECK(stateless_calls == 1);
    }

    // explain returns the query plan
    db << "CREATE TABLE planned (a INTEGER, b INTEGER)";
    db << "CREATE INDEX planned_a ON planned(a)";
    const auto plan = db.explain("SELECT b FROM planned WHERE a = 1");
    CHECK(!plan.empty());

    // connection diagnostics
    CHECK(db.cache_used() > 0);
    CHECK(db.schema_used() > 0);

    // load_extension reports a clear error for a missing library
    {
        bool load_failed = false;
        try {
            db.configure(sqlite::config_flag::load_extension, true);
            db.load_extension("/nonexistent/sqlitecpp-extension.so");
        } catch(const sqlite::sqlite_exception &) {
            load_failed = true;
        }
        db.configure(sqlite::config_flag::load_extension, false);
        CHECK(load_failed);
    }

    // file header diagnostics on a real on-disk database
    {
        const std::string path = (std::filesystem::temp_directory_path()
                                  / "sqlitecpp-header-test.db").string();
        std::remove(path.c_str());
        {
            sqlite::database file_db(path);
            file_db << "PRAGMA user_version = 42";
            file_db << "CREATE TABLE header_probe (v INTEGER)";
        }
        const auto info = sqlite::read_header(path);
        CHECK(info.valid());
        CHECK(info.user_version == 42);
        CHECK(info.page_size >= 512);
        CHECK(sqlite::has_sqlite_header(path));
        std::remove(path.c_str());
        CHECK(!sqlite::has_sqlite_header(path));
    }
    // window function: running total over an unbounded frame
    {
        struct running_total {
            long long total = 0;
        };
        db.define_window("wtot",
            [](running_total &state, long long value) { state.total += value; },
            [](running_total &state, long long value) { state.total -= value; },
            [](running_total &state) { return state.total; },
            [](running_total &state) { return state.total; });
        db << "CREATE TABLE win (v INTEGER)";
        db.execute_many("INSERT INTO win VALUES (?)", std::vector<std::tuple<int>>{{1}, {2}, {3}});

        std::vector<long long> totals;
        db << "SELECT wtot(v) OVER (ORDER BY v ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)"
              " FROM win" >> [&totals](long long total) { totals.push_back(total); };
        CHECK(totals.size() == 3 && totals[0] == 1 && totals[2] == 6);
    }

    // window function: sliding frame exercises xInverse
    {
        struct sliding_sum {
            long long total = 0;
        };
        db.define_window("wslide",
            [](sliding_sum &state, long long value) { state.total += value; },
            [](sliding_sum &state, long long value) { state.total -= value; },
            [](sliding_sum &state) { return state.total; },
            [](sliding_sum &state) { return state.total; });

        std::vector<long long> sliding;
        db << "SELECT wslide(v) OVER (ORDER BY v ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)"
              " FROM win" >> [&sliding](long long total) { sliding.push_back(total); };
        CHECK(sliding.size() == 3 && sliding[0] == 1 && sliding[1] == 3 && sliding[2] == 5);
    }
    // typed SQL literal: length known at compile time, empty rejected
    {
        using namespace sqlite::literals;
        int literal_value = 0;
        db << "SELECT 42"_sql >> literal_value;
        CHECK(literal_value == 42);

        CHECK("SELECT 1"_sql.view().size() == 8);
        CHECK(std::string_view("SELECT 1"_sql) == "SELECT 1");
    }
    // ---- binding lifecycle: cached statements never leak values ----------
    db << "CREATE TABLE lifecycle (a INTEGER, b TEXT)";
    db << "INSERT INTO lifecycle VALUES (?, ?)" << 1 << "first";
    db << "INSERT INTO lifecycle VALUES (?, ?)" << 2;     // b left unbound
    int unbound_is_null = 0;
    db << "SELECT b IS NULL FROM lifecycle WHERE a = 2" >> unbound_is_null;
    CHECK(unbound_is_null == 1);

    // execute_many with a narrower tuple NULLs the remaining parameters
    db.execute_many("INSERT INTO lifecycle VALUES (?, ?)",
                    std::vector<std::tuple<int>>{{3}, {4}});
    db << "SELECT COUNT(*) FROM lifecycle WHERE a IN (3, 4) AND b IS NULL" >> result;
    CHECK(result == 2);

    // a held binder can be rebound, with named parameters too
    {
        auto held = db << "INSERT INTO lifecycle VALUES (:a, :b)";
        held << sqlite::named_parameter(":a", 10) << sqlite::named_parameter(":b", "x");
        held.execute();
        held << sqlite::named_parameter(":a", 11) << sqlite::named_parameter(":b", "y");
        held.execute();
        held << sqlite::named_parameter(":a", 12);           // :b left unbound
        held.execute();
    }
    std::string held_b;
    db << "SELECT b FROM lifecycle WHERE a = 11" >> held_b;
    CHECK(held_b == "y");
    db << "SELECT b IS NULL FROM lifecycle WHERE a = 12" >> unbound_is_null;
    CHECK(unbound_is_null == 1);

    // re-running a round without rebinding keeps its parameters
    {
        auto rerun = db << "SELECT ? + 1" << 41;
        int first = 0;
        int second = 0;
        rerun >> first;
        rerun >> second;
        CHECK(first == 42 && second == 42);
    }

    // a failing execute() is not repeated by the destructor
    db << "CREATE TABLE unique_values (v INTEGER UNIQUE)";
    db << "INSERT INTO unique_values VALUES (1)";
    bool destructor_threw = false;
    try {
        auto duplicate = db << "INSERT INTO unique_values VALUES (1)";
        bool execute_threw = false;
        try {
            duplicate.execute();
        } catch(const sqlite::sqlite_exception &) {
            execute_threw = true;
        }
        CHECK(execute_threw);
    } catch(const std::exception &) {
        destructor_threw = true;
    }
    CHECK(!destructor_threw);

    // an abandoned iteration is reset on release and blocks nothing
    {
        auto partial = db << "SELECT a FROM lifecycle ORDER BY a";
        auto first_row = partial.begin(); // steps once, then abandoned
        CHECK(first_row != partial.end());
        auto moved = std::move(partial);
        (void)moved;
    }
    {
        sqlite::savepoint after_partial(db, "after_partial");
        after_partial.commit();
    }
    // ---- callbacks never unwind through SQLite's C frames ---------------
    {
        db.progress_handler([]() -> int { throw std::runtime_error("progress"); }, 1);
        bool interrupted = false;
        try {
            db << "SELECT COUNT(*) FROM lifecycle" >> result;
        } catch(const sqlite::errors::interrupt &) {
            interrupted = true;
        }
        db.clear_progress_handler();
        CHECK(interrupted);

        db.on_commit([]() -> int { throw std::runtime_error("commit"); });
        bool rolled_back = false;
        try {
            sqlite::transaction tx(db);
            db.execute("INSERT INTO lifecycle VALUES (100, 'never')");
            tx.commit();
        } catch(const sqlite::sqlite_exception &) {
            rolled_back = true;
        }
        db.on_commit({});
        CHECK(rolled_back);
        db << "SELECT COUNT(*) FROM lifecycle WHERE a = 100" >> result;
        CHECK(result == 0);

        db.on_update([](int, std::string_view, std::string_view, sqlite3_int64) {
            throw std::runtime_error("update");
        });
        db.execute("INSERT INTO lifecycle VALUES (101, 'kept')");
        db.on_update(nullptr);
        db << "SELECT COUNT(*) FROM lifecycle WHERE a = 101" >> result;
        CHECK(result == 1);
    }

    // ---- try_execute reports by code ------------------------------------
    {
        const auto duplicate_insert = db.try_execute("INSERT INTO unique_values VALUES (1)");
        CHECK(!duplicate_insert && duplicate_insert.code != SQLITE_OK);
        const auto changed = db.try_execute("INSERT INTO unique_values VALUES (?)", 2);
        CHECK(changed && changed.value() == 1);
    }

    // ---- used(false) re-runs the last round with its bindings -----------
    {
        db << "CREATE TABLE rerun (v INTEGER)";
        {
            auto again = db << "INSERT INTO rerun VALUES (?)" << 7;
            again.execute();
            again.used(false); // the destructor executes it once more
        }
        db << "SELECT COUNT(*) FROM rerun WHERE v = 7" >> result;
        CHECK(result == 2);
    }

    // ---- names passed as views need not be NUL-terminated ---------------
    {
        const std::string padded = "blobsXdataX";
        const std::string_view table(padded.data(), 5);   // "blobs", then 'X'
        const std::string_view column(padded.data() + 6, 4); // "data", then 'X'
        sqlite::blob handle(db, table, column, 1, true);
        CHECK(handle.size() == 1024);
        const std::string_view schema_padded = std::string_view("mainX").substr(0, 4);
        CHECK(!db.serialize(schema_padded).empty());
    }

    // ---- UTF-16 text round-trips through the 64-bit binding path ---------
    {
        std::u16string wide;
        db << "SELECT ?" << std::u16string_view(u"\u4e2d\u6587 text") >> wide;
        CHECK(wide == u"\u4e2d\u6587 text");
        std::string narrow;
        db << "SELECT ?" << u"utf16" >> narrow;
        CHECK(narrow == "utf16");
    }
}

int main() {
    try {
        run_suite();
    } catch(const std::exception &error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    std::puts("maintenance_regression: all checks passed");
    return 0;
}
