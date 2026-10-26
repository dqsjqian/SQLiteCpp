// libFuzzer target: throws arbitrary bytes at the SQL front end through the
// statement cache, binder and row iterator. Parse and runtime errors are the
// expected outcome; crashes, leaks, hangs or sanitizer reports are not.
#include <SQLiteCpp.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

int budget = 0;

sqlite::database &fuzz_database() {
    static sqlite::database db = [] {
        sqlite::database created(":memory:");
        sqlite3 *raw = created.connection().get();
        // Keep every input cheap and side-effect free: small values, no
        // attached files, no extension loading, defensive mode on.
        sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, 1 << 20);
        sqlite3_limit(raw, SQLITE_LIMIT_SQL_LENGTH, 1 << 16);
        sqlite3_limit(raw, SQLITE_LIMIT_ATTACHED, 0);
        created.configure(sqlite::config_flag::defensive, true);
        created.configure(sqlite::config_flag::load_extension, false);
        // A recursive CTE can loop forever; stop any statement after a fixed
        // amount of virtual-machine work.
        created.progress_handler([] { return ++budget > 10000 ? 1 : 0; }, 100);
        return created;
    }();
    return db;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    sqlite::database &db = fuzz_database();
    budget = 0;
    const std::string_view sql(reinterpret_cast<const char *>(data), size);
    try {
        auto statement = db << sql;
        for(auto &&row : statement) {
            std::string text;
            row >> text;
        }
    } catch(...) { // NOLINT(bugprone-empty-catch)
        // Rejected or failing input is the expected outcome.
    }
    return 0;
}
