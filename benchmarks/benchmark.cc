// Wrapper overhead against the raw SQLite C API.
//
// Every path does the same work on the same data: the row payloads are built
// once, before any timing, and every C++ path is matched by a C baseline
// that does exactly the same thing with a prepared statement it keeps. One
// warm-up run is discarded, the order of the paths rotates between runs and
// each figure is the median of the runs.
//
//   sqlitecpp_benchmark [runs] [--file]
//
// --file uses a database file in WAL mode instead of :memory:.
#include <SQLiteCpp.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
using rows_type = std::vector<std::tuple<int, std::string>>;

constexpr int row_count = 100000;
constexpr const char *create_sql = "CREATE TABLE bench (id INTEGER PRIMARY KEY, value TEXT)";
constexpr const char *insert_sql = "INSERT INTO bench VALUES (?, ?)";
constexpr const char *point_sql = "SELECT value FROM bench WHERE id = ?";
constexpr const char *scan_sql = "SELECT id, value FROM bench";

struct record {
    int id = 0;
    std::string value;
};

struct options {
    int runs = 7;
    bool file = false;
};

options parsed;

double ms_since(clock_type::time_point start) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}

void require(bool condition, const char *what) {
    if(!condition) {
        std::fprintf(stderr, "benchmark check failed: %s\n", what);
        std::exit(1);
    }
}

// A fresh database for every insert run, so no path inherits another's pages.
std::string fresh_path(const char *tag) {
    if(!parsed.file) return ":memory:";
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("sqlitecpp_bench_") + tag + ".db");
    for(const char *suffix : {"", "-wal", "-shm", "-journal"})
        std::filesystem::remove(path.string() + suffix);
    return path.string();
}

void prepare_c(sqlite3 *db) {
    if(parsed.file) {
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    }
    require(sqlite3_exec(db, create_sql, nullptr, nullptr, nullptr) == SQLITE_OK, "create");
}

sqlite::database open_cpp(const char *tag, std::size_t cache_size = 32) {
    sqlite::sqlite_config config;
    config.statement_cache_size = cache_size;
    sqlite::database db(fresh_path(tag), config);
    if(parsed.file) {
        require(db.wal(), "wal");
        db << "PRAGMA synchronous=NORMAL";
    }
    db << create_sql;
    return db;
}

// ---- INSERT --------------------------------------------------------------

double insert_c(const rows_type &rows) {
    sqlite3 *db = nullptr;
    sqlite3_open_v2(fresh_path("c").c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    prepare_c(db);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
    const auto start = clock_type::now();
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for(const auto &[id, value] : rows) {
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_bind_text64(stmt, 2, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        require(sqlite3_step(stmt) == SQLITE_DONE, "insert step");
        sqlite3_reset(stmt);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    const double elapsed = ms_since(start);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return elapsed;
}

double insert_per_row(const rows_type &rows, std::size_t cache_size, const char *tag) {
    auto db = open_cpp(tag, cache_size);
    const auto start = clock_type::now();
    db << "BEGIN";
    for(const auto &[id, value] : rows)
        db << insert_sql << id << value;
    db << "COMMIT";
    return ms_since(start);
}

double insert_held(const rows_type &rows) {
    auto db = open_cpp("held");
    auto insert = db << insert_sql;
    const auto start = clock_type::now();
    db << "BEGIN";
    for(const auto &[id, value] : rows) {
        insert << id << value;
        insert.execute();
    }
    db << "COMMIT";
    return ms_since(start);
}

double insert_many(const rows_type &rows) {
    auto db = open_cpp("many");
    const auto start = clock_type::now();
    sqlite::transaction tx(db);
    require(db.execute_many(insert_sql, rows) == row_count, "execute_many count");
    tx.commit();
    return ms_since(start);
}

// ---- reads -----------------------------------------------------------------

struct read_fixture {
    sqlite::database db;
    sqlite3 *handle;
    long long expected_point = 0; // sum of value lengths over every lookup
    long long expected_scan = 0;  // sum of id + value length over every row

    explicit read_fixture(const rows_type &rows): db(open_cpp("read")), handle(db.connection().get()) {
        sqlite::transaction tx(db);
        db.execute_many(insert_sql, rows);
        tx.commit();
        for(const auto &[id, value] : rows) {
            expected_point += static_cast<long long>(value.size());
            expected_scan += id + static_cast<long long>(value.size());
        }
    }
};

double point_c(read_fixture &f) {
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(f.handle, point_sql, -1, &stmt, nullptr);
    long long sum = 0;
    const auto start = clock_type::now();
    for(int id = 0; id < row_count; ++id) {
        sqlite3_bind_int(stmt, 1, id);
        if(sqlite3_step(stmt) == SQLITE_ROW) {
            std::string value(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)),
                              static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
            sum += static_cast<long long>(value.size());
        }
        sqlite3_reset(stmt);
    }
    const double elapsed = ms_since(start);
    sqlite3_finalize(stmt);
    require(sum == f.expected_point, "point C checksum");
    return elapsed;
}

// The same lookup with the guarantee `>>` gives for a single value: step
// again and require SQLITE_DONE, so a second row would be reported.
double point_c_checked(read_fixture &f) {
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(f.handle, point_sql, -1, &stmt, nullptr);
    long long sum = 0;
    const auto start = clock_type::now();
    for(int id = 0; id < row_count; ++id) {
        sqlite3_bind_int(stmt, 1, id);
        if(sqlite3_step(stmt) == SQLITE_ROW) {
            std::string value(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)),
                              static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
            sum += static_cast<long long>(value.size());
            require(sqlite3_step(stmt) == SQLITE_DONE, "point C single row");
        }
        sqlite3_reset(stmt);
    }
    const double elapsed = ms_since(start);
    sqlite3_finalize(stmt);
    require(sum == f.expected_point, "point C checked checksum");
    return elapsed;
}

double point_per_row(read_fixture &f) {
    long long sum = 0;
    const auto start = clock_type::now();
    for(int id = 0; id < row_count; ++id) {
        std::string value;
        f.db << point_sql << id >> value;
        sum += static_cast<long long>(value.size());
    }
    const double elapsed = ms_since(start);
    require(sum == f.expected_point, "point per-row checksum");
    return elapsed;
}

double point_held(read_fixture &f) {
    auto query = f.db << point_sql;
    long long sum = 0;
    const auto start = clock_type::now();
    for(int id = 0; id < row_count; ++id) {
        std::string value;
        query << id >> value;
        sum += static_cast<long long>(value.size());
    }
    const double elapsed = ms_since(start);
    require(sum == f.expected_point, "point held checksum");
    return elapsed;
}

double scan_c(read_fixture &f) {
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(f.handle, scan_sql, -1, &stmt, nullptr);
    long long sum = 0;
    const auto start = clock_type::now();
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        const int id = sqlite3_column_int(stmt, 0);
        std::string value(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)),
                          static_cast<std::size_t>(sqlite3_column_bytes(stmt, 1)));
        sum += id + static_cast<long long>(value.size());
    }
    const double elapsed = ms_since(start);
    sqlite3_finalize(stmt);
    require(sum == f.expected_scan, "scan C checksum");
    return elapsed;
}

double scan_callback(read_fixture &f) {
    long long sum = 0;
    const auto start = clock_type::now();
    f.db << scan_sql >> [&](int id, const std::string &value) {
        sum += id + static_cast<long long>(value.size());
    };
    const double elapsed = ms_since(start);
    require(sum == f.expected_scan, "scan callback checksum");
    return elapsed;
}

double scan_range(read_fixture &f) {
    long long sum = 0;
    const auto start = clock_type::now();
    for(auto &&row : f.db << scan_sql) {
        auto [id, value] = row.as<int, std::string>();
        sum += id + static_cast<long long>(value.size());
    }
    const double elapsed = ms_since(start);
    require(sum == f.expected_scan, "scan range checksum");
    return elapsed;
}

// select<Record> materialises a vector, so its baseline does too.
double collect_c(read_fixture &f) {
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(f.handle, scan_sql, -1, &stmt, nullptr);
    const auto start = clock_type::now();
    std::vector<record> out;
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        record r;
        r.id = sqlite3_column_int(stmt, 0);
        r.value.assign(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)),
                       static_cast<std::size_t>(sqlite3_column_bytes(stmt, 1)));
        out.push_back(std::move(r));
    }
    const double elapsed = ms_since(start);
    sqlite3_finalize(stmt);
    require(out.size() == static_cast<std::size_t>(row_count), "collect C size");
    return elapsed;
}

double collect_select(read_fixture &f) {
    const auto start = clock_type::now();
    const auto out = f.db.select<record>(scan_sql, &record::id, &record::value);
    const double elapsed = ms_since(start);
    require(out.size() == static_cast<std::size_t>(row_count), "select size");
    return elapsed;
}

// ---- reporting ---------------------------------------------------------------

struct series {
    const char *name;
    std::vector<double> samples;
    double median() const {
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }
};

void report(const char *title, const series &baseline, const std::vector<const series *> &paths) {
    std::printf("\n%s\n", title);
    const double base = baseline.median();
    std::printf("  %-36s %8.1f ms  %7.1f ns/op  baseline\n", baseline.name, base,
                base * 1e6 / row_count);
    for(const series *path : paths) {
        const double value = path->median();
        std::printf("  %-36s %8.1f ms  %7.1f ns/op  %+6.1f%%\n", path->name, value,
                    value * 1e6 / row_count, (value / base - 1.0) * 100.0);
    }
}

void run() {
    rows_type rows;
    rows.reserve(row_count);
    for(int i = 0; i < row_count; ++i) rows.emplace_back(i, "payload-" + std::to_string(i));

    series ins_c{"C API, held statement", {}};
    series ins_row{"db << sql << ... (statement cache)", {}};
    series ins_nocache{"db << sql << ... (cache disabled)", {}};
    series ins_held{"held binder, rebound per row", {}};
    series ins_many{"execute_many", {}};
    series pt_c{"C API, held statement", {}};
    series pt_c_checked{"C API, also stepping to SQLITE_DONE", {}};
    series pt_row{"db << sql << id >> value", {}};
    series pt_held{"held binder: query << id >> value", {}};
    series sc_c{"C API, step loop", {}};
    series sc_cb{"db << sql >> [](int, const string &)", {}};
    series sc_range{"for(auto &&row : db << sql) as<>", {}};
    series co_c{"C API into std::vector<record>", {}};
    series co_sel{"select<record>(sql, &record::...)", {}};

    using insert_path = std::pair<series *, std::function<double()>>;
    const std::vector<insert_path> inserts{
        {&ins_c, [&] { return insert_c(rows); }},
        {&ins_row, [&] { return insert_per_row(rows, 32, "row"); }},
        {&ins_nocache, [&] { return insert_per_row(rows, 0, "nocache"); }},
        {&ins_held, [&] { return insert_held(rows); }},
        {&ins_many, [&] { return insert_many(rows); }},
    };
    using read_path = std::pair<series *, double (*)(read_fixture &)>;
    const std::vector<read_path> reads{
        {&pt_c, point_c}, {&pt_c_checked, point_c_checked},
        {&pt_row, point_per_row}, {&pt_held, point_held},
        {&sc_c, scan_c}, {&sc_cb, scan_callback}, {&sc_range, scan_range},
        {&co_c, collect_c}, {&co_sel, collect_select},
    };

    // Run 0 only warms caches and the allocator. The starting path rotates
    // every run, so no path is always first or always last.
    for(int run = 0; run <= parsed.runs; ++run) {
        const bool record_run = run > 0;
        for(std::size_t i = 0; i < inserts.size(); ++i) {
            const auto &[target, measure] = inserts[(i + static_cast<std::size_t>(run)) % inserts.size()];
            const double elapsed = measure();
            if(record_run) target->samples.push_back(elapsed);
        }
        read_fixture fixture(rows);
        for(std::size_t i = 0; i < reads.size(); ++i) {
            const auto &[target, measure] = reads[(i + static_cast<std::size_t>(run)) % reads.size()];
            const double elapsed = measure(fixture);
            if(record_run) target->samples.push_back(elapsed);
        }
    }

    std::printf("SQLiteCpp %d.%d.%d, SQLite %s, %s, %d rows, median of %d runs\n",
                SQLITE_CPP_VERSION_MAJOR, SQLITE_CPP_VERSION_MINOR, SQLITE_CPP_VERSION_PATCH,
                sqlite3_libversion(), parsed.file ? "file (WAL)" : ":memory:", row_count, parsed.runs);
    report("INSERT, one transaction", ins_c, {&ins_row, &ins_nocache, &ins_held, &ins_many});
    report("Point lookup by primary key, TEXT result", pt_c, {&pt_c_checked, &pt_row, &pt_held});
    report("Full scan, INTEGER + TEXT per row", sc_c, {&sc_cb, &sc_range});
    report("Full scan into std::vector<record>", co_c, {&co_sel});
}

} // namespace

int main(int argc, char **argv) {
    for(int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        int runs = 0;
        const auto [end, error] = std::from_chars(arg.data(), arg.data() + arg.size(), runs);
        if(arg == "--file") {
            parsed.file = true;
        } else if(error == std::errc() && end == arg.data() + arg.size() && runs > 0) {
            parsed.runs = runs;
        } else {
            std::fprintf(stderr, "usage: sqlitecpp_benchmark [runs] [--file]\n");
            return 2;
        }
    }
    try {
        run();
    } catch(const std::exception &error) {
        std::fprintf(stderr, "benchmark failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
