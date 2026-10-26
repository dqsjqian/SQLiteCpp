// libFuzzer target: throws arbitrary bytes at the SQL front end. Parse errors
// are the expected outcome; crashes, leaks or sanitizer reports are not.
#include <SQLiteCpp.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    static sqlite::database db(":memory:");
    const std::string_view sql(reinterpret_cast<const char *>(data), size);
    try {
        auto statement = db << sql;
        statement.execute();
    } catch(...) {
        // Rejected input is fine.
    }
    return 0;
}
