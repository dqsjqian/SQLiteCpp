// One sqlite::database shared by several threads. SQLite runs in serialized
// mode, so the connection itself is safe to share; the wrapper's statement
// cache must be too. Run under ThreadSanitizer in CI.
#include <SQLiteCpp.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <thread>
#include <vector>

static int run() {
    sqlite::database db(":memory:");
    db << "CREATE TABLE shared (thread INTEGER, value INTEGER)";

    constexpr int threads = 4;
    constexpr int rows_per_thread = 500;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for(int t = 0; t < threads; ++t) {
        workers.emplace_back([&db, &failures, t] {
            try {
                for(int i = 0; i < rows_per_thread; ++i) {
                    db << "INSERT INTO shared VALUES (?, ?)" << t << i;
                    int count = 0;
                    db << "SELECT COUNT(*) FROM shared WHERE thread = ?" << t >> count;
                    if(count != i + 1) ++failures;
                }
            } catch(const std::exception &error) {
                std::fprintf(stderr, "thread %d: %s\n", t, error.what());
                ++failures;
            }
        });
    }
    for(auto &worker : workers) worker.join();

    int total = 0;
    db << "SELECT COUNT(*) FROM shared" >> total;
    if(failures != 0 || total != threads * rows_per_thread) {
        std::fprintf(stderr, "concurrency: failures=%d total=%d\n", failures.load(), total);
        return 1;
    }
    std::puts("concurrency: OK");
    return 0;
}

int main() {
    try {
        return run();
    } catch(const std::exception &error) {
        std::fprintf(stderr, "concurrency: %s\n", error.what());
        return 1;
    }
}
