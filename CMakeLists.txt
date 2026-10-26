cmake_minimum_required(VERSION 3.21)
project(SQLiteCpp VERSION 1.1.1 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- the library: header-only ------------------------------------------------
add_library(SQLiteCpp INTERFACE)
add_library(SQLiteCpp::SQLiteCpp ALIAS SQLiteCpp)
set_target_properties(SQLiteCpp PROPERTIES EXPORT_NAME SQLiteCpp)
target_compile_features(SQLiteCpp INTERFACE cxx_std_23)
target_include_directories(SQLiteCpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# ---- install -----------------------------------------------------------------
install(TARGETS SQLiteCpp EXPORT SQLiteCppTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
install(EXPORT SQLiteCppTargets NAMESPACE SQLiteCpp:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SQLiteCpp)
configure_package_config_file(cmake/SQLiteCppConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/SQLiteCppConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SQLiteCpp
    PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/SQLiteCppConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SQLiteCppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/SQLiteCppConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SQLiteCpp)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/SQLiteCpp)

# Ship the bundled SQLite source with the package: an installed SQLiteCpp is a
# complete unit, so consumers can build it without hunting for a SQLite.
install(FILES vendor/sqlite/sqlite3.c vendor/sqlite/sqlite3.h vendor/sqlite/sqlite3ext.h
        vendor/sqlite/README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/SQLiteCpp/sqlite)
configure_file(cmake/SQLiteCpp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/SQLiteCpp.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SQLiteCpp.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---- tests, benchmark, fuzzer --------------------------------------------------
option(SQLITECPP_WARNINGS_AS_ERRORS "Build tests and tools with strict warnings as errors" OFF)
option(SQLITECPP_COVERAGE "Instrument tests for coverage (GCC/Clang)" OFF)
option(SQLITECPP_BUILD_FUZZERS "Build the libFuzzer target (Clang with libFuzzer only)" OFF)

include(CTest)
if(BUILD_TESTING)
    # SQLite comes from vendor/sqlite only: one version, the newest. It is
    # third-party code, so it never takes the strict warning flags below.
    add_library(sqlite3_core STATIC vendor/sqlite/sqlite3.c)
    # Column metadata enables sqlite3_column_table_name / _origin_name.
    target_compile_definitions(sqlite3_core PUBLIC SQLITE_ENABLE_COLUMN_METADATA=1)
    target_include_directories(sqlite3_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite>)
    if(MSVC)
        target_compile_options(sqlite3_core PRIVATE /w)
    else()
        target_compile_options(sqlite3_core PRIVATE -w)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

    # Options shared by every C++ target this project builds for itself.
    add_library(sqlitecpp_build_options INTERFACE)
    target_compile_features(sqlitecpp_build_options INTERFACE cxx_std_23)
    if(SQLITECPP_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(sqlitecpp_build_options INTERFACE /W4 /WX /permissive-)
        else()
            target_compile_options(sqlitecpp_build_options INTERFACE
                -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
    if(SQLITECPP_COVERAGE)
        if(MSVC)
            message(FATAL_ERROR "SQLITECPP_COVERAGE needs GCC or Clang")
        endif()
        target_compile_options(sqlitecpp_build_options INTERFACE --coverage -O0 -g)
        target_link_options(sqlitecpp_build_options INTERFACE --coverage)
    endif()

    function(sqlitecpp_add_program name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE
            SQLiteCpp::SQLiteCpp sqlite3_core sqlitecpp_build_options)
    endfunction()

    sqlitecpp_add_program(sqlitecpp_regression tests/maintenance_regression.cpp)
    target_compile_definitions(sqlitecpp_regression PRIVATE SQLITECPP_BUNDLED_SQLITE=1)
    add_test(NAME sqlitecpp-maintained-api COMMAND sqlitecpp_regression)

    set(SQLITECPP_REGRESSION_TESTS
        blob_example error_log error_log2 exception_dont_execute
        exception_dont_execute_nested exceptions extended_exceptions
        functions functors lvalue_functor_example mov_ctor nullptr_uniqueptr
        readme_example shared_connection simple_examples
        std_optional trycatchblocks variant concurrency)
    foreach(test_name IN LISTS SQLITECPP_REGRESSION_TESTS)
        sqlitecpp_add_program(${test_name} tests/${test_name}.cc)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    sqlitecpp_add_program(sqlitecpp_benchmark benchmarks/benchmark.cc)

    if(SQLITECPP_BUILD_FUZZERS)
        # Needs the libFuzzer runtime: Linux Clang ships it; AppleClang and
        # MSVC do not.
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "SQLITECPP_BUILD_FUZZERS needs LLVM Clang with libFuzzer")
        endif()
        sqlitecpp_add_program(sqlitecpp_fuzz_sql fuzz/fuzz_sql.cc)
        target_compile_options(sqlitecpp_fuzz_sql PRIVATE -fsanitize=fuzzer,address,undefined -g)
        target_link_options(sqlitecpp_fuzz_sql PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endif()
