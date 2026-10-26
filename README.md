# SQLiteCpp

The most advanced modern C++23 wrapper for SQLite — stream-style parameter
binding, typed results, optional values, named parameters and row iteration.

## Usage

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

## Requirements

The headers require C++23 and a separately provided SQLite library. Standard
optional, variant and string-view support is always enabled.

## Build & Install

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix /path/to/sdk
```

Installed consumers can use `find_package(SQLiteCpp CONFIG REQUIRED)` and link
`SQLiteCpp::SQLiteCpp` together with their SQLite target. SQLite itself is not
bundled. ForgeRecipes can supply both libraries through a ForgeDeps workspace.
Tests use a caller-provided SQLite installation.

## License

Released under the MIT license. See [License.txt](License.txt) for details.
