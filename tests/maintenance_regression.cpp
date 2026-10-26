#include <SQLiteCpp.h>
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
int main() {
    sqlite::database db(":memory:");
    db << "CREATE TABLE item (id INTEGER PRIMARY KEY, value TEXT, missing INTEGER, data BLOB)";
    const std::string_view text = "hello";
    const std::vector<unsigned char> data{0, 1, 255};
    db << "INSERT INTO item VALUES (?, ?, ?, ?)" << 1 << text << std::optional<int>{} << data;
    std::string found;
    std::optional<int> absent;
    std::vector<unsigned char> blob;
    db << "SELECT value, missing, data FROM item WHERE id=:id" << sqlite::named_parameter(":id", 1)
       >> [&](std::string value, std::optional<int> missing, std::vector<unsigned char> bytes) {
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
}
