# SQLiteCpp

简体中文 | [English](README.en.md)

最先进的现代 C++23 SQLite 封装库：流式参数绑定、强类型结果、结构化行映射、预编译语句缓存。
纯头文件、自闭环、只认一个 SQLite 版本。

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

## 核心特性

- **预编译语句缓存**：重复 SQL 只编译一次（`sqlite3_prepare_v3` + `SQLITE_PREPARE_PERSISTENT`），之后重置复用。容量由 `sqlite_config::statement_cache_size` 控制（默认 32，`0` 关闭缓存）。正在执行的语句绝不会被二次借出，同一条 SQL 嵌套使用依然正确。
- **多线程安全共享**：缓存使用连接自身的 SQLite 互斥锁，一个 `sqlite::database` 可以按 SQLite 允许的方式（串行化模式）被多个线程共享。已在 ThreadSanitizer 下验证。
- **可预期的绑定语义**：每次执行都从干净状态开始。没有绑定的参数一律读作 `NULL`，即使语句来自缓存；持有的语句可以反复重新绑定、重新执行。
- **RAII 事务**：`sqlite::transaction` 离开作用域时若未 `commit()` 自动回滚，支持 `deferred` / `immediate` / `exclusive`；嵌套的 `sqlite::savepoint` 同理。
- **批量执行**：`execute_many` 对区间中每个元组绑定一次，同一语句编译一次执行 N 次，与手写 C 循环持平。
- **结构化结果**：通过成员指针把列映射到结构体，或读成 tuple 配合结构化绑定。
- **无异常路径**：`try_execute` 为 `noexcept`，失败通过 `sqlite::execution_result`（影响行数、错误码、错误信息）按值返回。
- **精确错误信息**：异常携带结果码、扩展码、SQL 文本，以及出错位置在 SQL 中的字节偏移（`sqlite3_error_offset`）。
- **并发与控制**：`busy_timeout`、`configure`（外键、防御模式等）、`interrupt`、进度回调。
- **诊断能力**：`explain` 查询计划、`cache_used` / `schema_used`、无需打开连接即可解析 100 字节文件头的 `read_header`，以及 update / commit / rollback 钩子和 `load_extension`。
- **数据通道**：`blob` 分块读写大 BLOB，`no_copy` 以 `SQLITE_STATIC` 零拷贝绑定，`serialize` / `deserialize` 整库内存镜像，每条语句都能读取列元数据（列名、声明类型、来源表）。所有长度统一走 SQLite 64 位接口。
- **SQL 函数**：标量、聚合、窗口函数，直接用普通 lambda 注册。
- **全面 C++23**：`std::string_view`、`std::optional`、`std::variant`、`std::span`、`sqlite::bindable` concept、`[[nodiscard]]` 访问器，低于 C++23 直接 `#error`。
- **内置 SQLite，零构建依赖**：`vendor/sqlite` 内置官方 SQLite 3.54.1 amalgamation（已对照 sqlite.org 校验 SHA3-256）。克隆、配置、构建即可，不需要包管理器、不需要下载工具链、不需要联网。

## API 总览

| 领域 | 入口 |
|---|---|
| 连接 | `database`、`sqlite_config`、`config_flag`、`busy_timeout`、`configure`、`interrupt`、`progress_handler`、`load_extension` |
| 语句 | `operator<<`、`operator>>`、`execute`、`execute_many`、`try_execute`、`row.as<Ts...>()`、`column_name` / `column_declared_type` / `column_table_name` |
| 事务 | `transaction`、`savepoint` |
| 结果 | `select<Record>(...)`、`as<Ts...>()`、`pragma`、`explain` |
| 数据 | `blob`（增量 I/O）、`no_copy`（`SQLITE_STATIC`）、`serialize` / `deserialize`、`backup` |
| 函数 | `define`（标量 / 聚合 / 标志位）、`define_window` |
| 文件 | `read_header`、`has_sqlite_header` |
| 钩子 | `on_update`、`on_commit`、`on_rollback` |
| 字面量 | `"SELECT 1"_sql` |
| 诊断 | `cache_used`、`schema_used` |

## 批量插入与事务

```cpp
const std::vector<std::tuple<int, std::string>> rows{{1, "one"}, {2, "two"}};

sqlite::transaction tx(db, sqlite::transaction_behavior::immediate);
db.execute_many("INSERT INTO item VALUES (?, ?)", rows);
tx.commit();      // 不调用 commit()，离开作用域时自动回滚
```

## 结构化读取

```cpp
struct item_record {
    int id = 0;
    std::string value;
};

const auto items = db.select<item_record>(
    "SELECT id, value FROM item ORDER BY id",
    &item_record::id, &item_record::value);

for(auto &&row : db << "SELECT id, value FROM item") {
    auto [id, text] = row.as<int, std::string>();
}
```

## 复用语句

```cpp
auto insert = db << "INSERT INTO item VALUES (?, ?)";
for(const auto &[id, text] : rows) {
    insert << id << text;
    insert.execute();      // 执行后自动重置，下一次 << 从 ?1 开始
}
```

本轮未绑定的参数一律为 `NULL`，绝不会残留同一条缓存 SQL 上一次执行时的值。

## 多线程

SQLite 运行在串行化模式时（SQLite 默认模式，也是内置 amalgamation 的模式），一个 `sqlite::database` 可以在多个线程间共享。语句缓存会获取连接自身的互斥锁，因此并发调用 `db << ...` 是安全的。

使用约束：

- 单个语句对象（`db << sql` 返回的值）同一时刻只属于一个线程。
- 钩子和进度回调应在连接被共享之前安装。
- 以 `OpenFlags::NOMUTEX` 打开的连接没有互斥锁，同一时刻只能由一个线程使用，缓存也因此完全不付加锁开销。

## 钩子

```cpp
db.on_update([](int action, std::string_view database, std::string_view table,
                sqlite3_int64 rowid) {
    // database / table 指向 SQLite 内部内存，需要保留请自行拷贝
});
db.on_update(nullptr);     // 移除钩子
```

参数为 `std::string` 的旧写法依然可用。钩子抛出的异常绝不会穿过 SQLite 的 C 栈帧：进度回调抛异常会中断当前语句，commit 钩子抛异常会把提交转为回滚。

## 性能

`benchmarks/benchmark.cc`，Release 构建，Apple Silicon，内置 SQLite 3.54.1，每项 10 万次操作。每条路径做同样的工作：行数据在计时开始前一次性构造好，C 基线同样持有一条预编译语句；第一轮只预热并丢弃，各路径的先后顺序每轮轮转，取 9 轮中位数。

`:memory:`：

| 路径 | 耗时 | 单次操作 | 对比裸 C API |
|---|---|---|---|
| **写入**（一个事务） | | | |
| 裸 C API，同一条预编译语句复用 | 20.7 ms | 207 ns | 基准 |
| 逐行 `db << sql << ...`，语句缓存（默认） | 24.0 ms | 240 ns | +16% |
| 逐行 `db << sql << ...`，关闭缓存 | 136.0 ms | 1.36 µs | +557%（慢 6.6 倍） |
| 持有语句，逐行重新绑定 | 21.2 ms | 212 ns | +2% |
| `execute_many` | 20.5 ms | 205 ns | -1%（持平） |
| **按主键单行查询**（结果为 TEXT） | | | |
| 裸 C API，同一条预编译语句复用 | 22.9 ms | 229 ns | 基准 |
| 裸 C API，并再 step 一次到 `SQLITE_DONE` | 25.0 ms | 250 ns | +9% |
| 逐行 `db << sql << id >> value` | 27.8 ms | 278 ns | +21% |
| 持有语句 `query << id >> value` | 25.7 ms | 257 ns | +12% |
| **全表扫描**（每行 INTEGER + TEXT） | | | |
| 裸 C API，step 循环 | 7.1 ms | 71 ns | 基准 |
| `db << sql >> [](int, const std::string &)` | 7.4 ms | 74 ns | +4% |
| `for (auto &&row : db << sql) row.as<int, std::string>()` | 7.3 ms | 73 ns | +3% |
| `select<record>(...)`（裸 C 侧同样存进 vector） | 7.6 ms | 76 ns | +1% |

同一套基准换成 WAL 模式的磁盘库，比例不变：逐行写入 +10%、持有语句 -3%、`execute_many` -2%、单行查询 +8%（其中 4 个百分点是单行校验本身）、扫描 +3% 至 -1%。

怎么读这张表：

- **逐行表达式语法每行约 33 ns**：一个语句对象、一次缓存查找（一次比较即命中）、一次连接锁。这就是 `execute_many` 和持有语句能省掉的全部。
- **单行 `>>` 的 +21% 里有 9 个百分点是语义，不是开销**：它要多执行一次 `step` 才能断言"结果确实只有一行"，否则立刻抛 `more_rows`。裸 C 循环若做同样的检查，代价同样是 +9%。剩下的 12 个百分点是缓存查找与语句对象。
- **扫描几乎没有开销**：每个列值只调用一次 SQLite（SQL NULL 由 SQLite 自身的结果表达，不再额外查一次类型），三列读取的路径与手写 C 循环相差 1 至 4 ns/行。
- **`execute_many` 与持有语句持平，不比裸 C 快**：1.1.0 曾宣称"快 8%"，那是基准不对等造成的——当时 `execute_many` 的行数据在计时外构造，C 基线却在计时内拼字符串。同一口径下二者相等，提交一次、循环 N 次就是裸 C 的上限。

对比 1.1.1（同一台机器、同一套基准）：逐行写入 +24% → +16%，单行查询 +36% → +21%，全表扫描 +29% → +3%。

## 平台支持

所有任务都以警告即错误构建（GCC/Clang 为 `-Wall -Wextra -Wpedantic -Werror`，MSVC 与 clang-cl 为 `/W4 /WX /permissive-`），运行在 GitHub 的 `-latest` 镜像上，编译器一律使用最新稳定版，不依赖镜像预装的旧版本：GCC 16 取自官方 `gcc:16` 镜像，LLVM/Clang 23 在 Linux 上取自 LLVM 官方 apt 源、在 Windows 上取自 LLVM 官方发布包。

| 平台 | Runner | 工具链 | 验证内容 |
|---|---|---|---|
| Linux x86-64 | `ubuntu-latest` | GCC 16；Clang 23（libstdc++ 与 libc++），Debug 与 Release | 构建、测试、安装 |
| Linux ARM64 | `ubuntu-26.04-arm` | GCC 16，原生运行 | 构建、测试 |
| macOS | `macos-latest` | AppleClang（镜像上最新的正式版 Xcode） | 构建、测试、安装 |
| iOS | `macos-latest` | AppleClang（同上），模拟器 arm64 | 构建 |
| Android | `ubuntu-latest` | NDK r30（镜像上最新），arm64-v8a 与 armeabi-v7a | 构建 |
| Windows x64 | `windows-latest` | MSVC（Visual Studio 2026）；clang-cl 23 | 构建、测试 |
| Windows ARM64 | `windows-11-vs2026-arm` | MSVC（Visual Studio 2026），原生运行 | 构建、测试 |
| Windows MinGW | `windows-latest` | MSYS2 UCRT64 GCC（随 MSYS2 更新，当前 16.2） | 构建、测试 |

每次推送的质量门禁：AddressSanitizer + UndefinedBehaviorSanitizer（GCC 16）、ThreadSanitizer（Clang 23）、clang-tidy 23（缺陷类检查，发现即失败）、`include/` 的 lcov 覆盖率、针对 SQL 前端的 60 秒 libFuzzer、Doxygen API 文档、每个公开头文件单独编译。

## 环境要求

头文件要求 C++23 与 SQLite 3.38 及以上。仓库在 `vendor/sqlite` 内置官方 SQLite 3.54.1 amalgamation，测试始终使用这一份。安装包同样附带这份 amalgamation，路径由 `SQLiteCpp_SQLITE_DIR` 给出。使用方若链接自己的 SQLite，版本必须不低于 3.38，更低的版本会在编译期给出明确报错。

## 构建与安装

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --prefix /path/to/sdk
```

```cmake
project(app LANGUAGES C CXX)             # sqlite3.c 需要 C 语言支持
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(sqlite3_core STATIC ${SQLiteCpp_SQLITE_DIR}/sqlite3.c)
target_include_directories(sqlite3_core PUBLIC ${SQLiteCpp_SQLITE_DIR})
target_link_libraries(sqlite3_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

target_link_libraries(app PRIVATE SQLiteCpp::SQLiteCpp sqlite3_core)
```

| CMake 选项 | 默认值 | 作用 |
|---|---|---|
| `BUILD_TESTING` | `ON`（CTest） | 构建回归测试与基准测试 |
| `SQLITECPP_WARNINGS_AS_ERRORS` | `OFF` | 测试与工具以严格警告即错误构建 |
| `SQLITECPP_COVERAGE` | `OFF` | 开启 `--coverage` 覆盖率插桩（GCC / Clang） |
| `SQLITECPP_BUILD_FUZZERS` | `OFF` | 构建 libFuzzer 目标（LLVM Clang） |

## 目录结构

| 路径 | 内容 |
|---|---|
| `include/SQLiteCpp.h` | 唯一包含入口 |
| `include/SQLiteCpp/config.h` | 版本宏、C++23 要求 |
| `include/SQLiteCpp/types.h` | 字符串引用类型 |
| `include/SQLiteCpp/errors.h` | 异常体系 |
| `include/SQLiteCpp/fwd.h` | 前置声明、标志位、`execution_result` |
| `include/SQLiteCpp/binder.h` | 语句、绑定、行迭代 |
| `include/SQLiteCpp/database.h` | 连接，批量 / 快照 / pragma / 钩子接口 |
| `include/SQLiteCpp/transaction.h` | `transaction`、`savepoint` |
| `include/SQLiteCpp/sql_function.h` | 标量、聚合、窗口函数 |
| `include/SQLiteCpp/backup.h` | 连接间在线备份 |
| `include/SQLiteCpp/blob.h` | BLOB 增量读写 |
| `include/SQLiteCpp/diagnostics.h` | 数据库文件头解析 |
| `include/SQLiteCpp/literals.h` | `_sql` 字面量 |
| `include/SQLiteCpp/detail/` | 值类型适配、语句缓存、UTF 转换 |
| `vendor/sqlite/` | 内置 SQLite 3.54.1 amalgamation |
| `tests/` | 回归测试、多线程测试、安装后消费端工程 |
| `benchmarks/` | 对比裸 C API 的微基准 |
| `fuzz/` | libFuzzer 目标 |
| `docs/` | Doxygen 配置 |
| `.github/` | CI 工作流与工具链安装脚本 |

## 许可证

基于 MIT 许可证发布，详见 [LICENSE](LICENSE)。
