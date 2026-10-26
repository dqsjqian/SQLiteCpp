cmake_minimum_required(VERSION 3.21)
project(InstalledConsumer LANGUAGES C CXX)

# Verifies that an installed SQLiteCpp package is consumable. The consumer
# supplies SQLite itself; this project compiles either the vendored
# amalgamation shipped inside the package or the repository copy.
find_package(SQLiteCpp CONFIG REQUIRED)

if(SQLiteCpp_SQLITE_DIR)
    set(_sqlite_dir "${SQLiteCpp_SQLITE_DIR}")
else()
    set(_sqlite_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/sqlite")
endif()

add_library(sqlite3_core STATIC "${_sqlite_dir}/sqlite3.c")
target_include_directories(sqlite3_core PUBLIC "${_sqlite_dir}")

add_executable(consumer ../maintenance_regression.cpp)
target_compile_features(consumer PRIVATE cxx_std_23)
target_link_libraries(consumer PRIVATE SQLiteCpp::SQLiteCpp sqlite3_core)

enable_testing()
add_test(NAME installed-consumer COMMAND consumer)
