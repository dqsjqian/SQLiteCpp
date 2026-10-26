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

The headers require C++23. The repository vendors the official SQLite 3.54.1
amalgamation in `vendor/sqlite`, and tests build and link it by default. Pass
`-DSQLITECPP_BUNDLE_SQLITE=OFF` to link an external SQLite instead. Installed
consumers supply their own SQLite library.

## Build & Install

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix /path/to/sdk
```

The full regression suite (C++23, bundled SQLite 3.54.1) runs under `ctest`.
Installed consumers can use `find_package(SQLiteCpp CONFIG REQUIRED)` and link
`SQLiteCpp::SQLiteCpp` together with their SQLite target. ForgeRecipes can
supply both libraries through a ForgeDeps workspace.

## License

Released under the MIT license. See [License.txt](License.txt) for details.
