// Micro-benchmark for the execution path: repeated prepare vs statement reuse.
// Build: cmake --build build --target sqlitecpp_benchmark
#include <SQLiteCpp.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using clock_type = std::chrono::steady_clock;

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
    return 0;
}
