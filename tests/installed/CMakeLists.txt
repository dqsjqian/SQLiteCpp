cmake_minimum_required(VERSION 3.21)
project(InstalledConsumer LANGUAGES CXX)

# Verifies that an installed SQLiteCpp package is usable through
# find_package() with a caller-provided SQLite.
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(consumer ../maintenance_regression.cpp)
target_compile_features(consumer PRIVATE cxx_std_23)
target_link_libraries(consumer PRIVATE SQLiteCpp::SQLiteCpp SQLite::SQLite3)

enable_testing()
add_test(NAME installed-consumer COMMAND consumer)
