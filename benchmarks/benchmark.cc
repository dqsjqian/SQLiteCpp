// Micro-benchmark for the execution path: repeated prepare vs statement reuse.
// Build: cmake --build build --target sqlitecpp_benchmark
#include <SQLiteCpp.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using clock_type = std::chrono::steady_clock;

static double ms_since(clock_type::time_point start);

// Baseline: the same workload straight against the C API, one prepared
// statement reused, so the numbers isolate wrapper overhead.
struct raw_timing {
    double insert_ms = 0;
    double select_ms = 0;
    long long checksum = 0;
};

static raw_timing bench_raw_c(int inserts, int selects) {
    raw_timing result;
    sqlite3 *db = nullptr;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db, "CREATE TABLE bench (id INTEGER PRIMARY KEY, value TEXT)", nullptr, nullptr, nullptr);

    sqlite3_stmt *insert_stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO bench VALUES (?, ?)", -1, &insert_stmt, nullptr);
    const auto insert_start = clock_type::now();
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for(int i = 0; i < inserts; ++i) {
        const std::string payload = "payload-" + std::to_string(i);
        sqlite3_bind_int(insert_stmt, 1, i);
        sqlite3_bind_text(insert_stmt, 2, payload.c_str(),
                          static_cast<int>(payload.size()), SQLITE_TRANSIENT);
        sqlite3_step(insert_stmt);
        sqlite3_reset(insert_stmt);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    result.insert_ms = ms_since(insert_start);
    sqlite3_finalize(insert_stmt);

    sqlite3_stmt *select_stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM bench WHERE id = ?", -1, &select_stmt, nullptr);
    const auto select_start = clock_type::now();
    for(int i = 0; i < selects; ++i) {
        sqlite3_bind_int(select_stmt, 1, i % inserts);
        if(sqlite3_step(select_stmt) == SQLITE_ROW)
            result.checksum += sqlite3_column_int(select_stmt, 0);
        sqlite3_reset(select_stmt);
    }
    result.select_ms = ms_since(select_start);
    sqlite3_finalize(select_stmt);
    sqlite3_close(db);
    return result;
}

static double ms_since(clock_type::time_point start) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}

int main() {
    const int inserts = 100000;
    const int selects = 100000;

    sqlite::database db(":memory:");
    db << "CREATE TABLE bench (id INTEGER PRIMARY KEY, value TEXT)";

    auto t0 = clock_type::now();
    db << "BEGIN";
    for (int i = 0; i < inserts; ++i) {
        db << "INSERT INTO bench VALUES (?, ?)" << i << "payload-" + std::to_string(i);
    }
    db << "COMMIT";
    const double insert_ms = ms_since(t0);

    auto t1 = clock_type::now();
    long long sum = 0;
    for (int i = 0; i < selects; ++i) {
        int id = 0;
        db << "SELECT id FROM bench WHERE id = ?" << (i % inserts) >> id;
        sum += id;
    }
    const double select_ms = ms_since(t1);

    std::printf("inserts=%d  %.1f ms  (%.2f us/op)\n", inserts, insert_ms, insert_ms * 1000.0 / inserts);
    std::printf("selects=%d  %.1f ms  (%.2f us/op)\n", selects, select_ms, select_ms * 1000.0 / selects);
    std::printf("checksum=%lld\n", sum);

    // execute_many: one compile, N executions (row payloads pre-built)
    std::vector<std::tuple<int, std::string>> prepared_rows;
    prepared_rows.reserve(inserts);
    for (int i = 0; i < inserts; ++i) {
        prepared_rows.emplace_back(i, "payload-" + std::to_string(i));
    }
    sqlite::database batch_db(":memory:");
    batch_db << "CREATE TABLE bench_batch (id INTEGER PRIMARY KEY, value TEXT)";
    auto t2 = clock_type::now();
    sqlite::transaction tx(batch_db);
    const auto batch_count = batch_db.execute_many("INSERT INTO bench_batch VALUES (?, ?)", prepared_rows);
    tx.commit();
    const double batch_ms = ms_since(t2);
    std::printf("execute_many inserts=%lld  %.1f ms  (%.2f us/op)\n",
                static_cast<long long>(batch_count), batch_ms, batch_ms * 1000.0 / inserts);

    // Reused binder: the exact equivalent of holding a C statement. No cache
    // lookup and no binder construction per row.
    sqlite::database reuse_db(":memory:");
    reuse_db << "CREATE TABLE bench (id INTEGER PRIMARY KEY, value TEXT)";
    auto reused = reuse_db << "INSERT INTO bench VALUES (?, ?)";
    const auto reuse_start = clock_type::now();
    reuse_db << "BEGIN";
    for(int i = 0; i < inserts; ++i) {
        const std::string payload = "payload-" + std::to_string(i);
        reused << i << payload;
        reused.execute();
    }
    reuse_db << "COMMIT";
    const double reuse_ms = ms_since(reuse_start);
    std::printf("reused binder  inserts=%d  %.1f ms  (%.2f us/op)\n",
                inserts, reuse_ms, reuse_ms * 1000.0 / inserts);

    // Wrapper overhead against the C API baseline.
    const auto raw = bench_raw_c(inserts, selects);
    std::printf("\nraw C API      inserts=%.1f ms  selects=%.1f ms\n", raw.insert_ms, raw.select_ms);
    std::printf("SQLiteCpp      inserts=%.1f ms  selects=%.1f ms\n", insert_ms, select_ms);
    std::printf("overhead       inserts=%+.1f%%  selects=%+.1f%%\n",
                (insert_ms - raw.insert_ms) / raw.insert_ms * 100.0,
                (select_ms - raw.select_ms) / raw.select_ms * 100.0);
    std::printf("reused binder  overhead=%+.1f%%\n",
                (reuse_ms - raw.insert_ms) / raw.insert_ms * 100.0);
    return 0;
}
