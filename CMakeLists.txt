cmake_minimum_required(VERSION 3.21)
project(SQLiteCpp VERSION 1.0.0 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
add_library(SQLiteCpp INTERFACE)
add_library(SQLiteCpp::SQLiteCpp ALIAS SQLiteCpp)
set_target_properties(SQLiteCpp PROPERTIES EXPORT_NAME SQLiteCpp)
target_compile_features(SQLiteCpp INTERFACE cxx_std_23)
target_include_directories(SQLiteCpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
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
include(CTest)
if(BUILD_TESTING)
    # SQLite comes from vendor/sqlite only: one version, the newest.
    add_library(sqlite3_core STATIC vendor/sqlite/sqlite3.c)
    target_include_directories(sqlite3_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite>)
    if(MSVC)
        target_compile_options(sqlite3_core PRIVATE /w)
    else()
        target_compile_options(sqlite3_core PRIVATE -w)
    endif()
    set(SQLITECPP_SQLITE_TARGET sqlite3_core)

    add_executable(sqlitecpp_regression tests/maintenance_regression.cpp)
    target_compile_features(sqlitecpp_regression PRIVATE cxx_std_23)
    target_link_libraries(sqlitecpp_regression PRIVATE SQLiteCpp::SQLiteCpp ${SQLITECPP_SQLITE_TARGET})
    target_compile_definitions(sqlitecpp_regression PRIVATE SQLITECPP_BUNDLED_SQLITE=1)
    add_test(NAME sqlitecpp-maintained-api COMMAND sqlitecpp_regression)

    add_executable(sqlitecpp_benchmark benchmarks/benchmark.cc)
    target_compile_features(sqlitecpp_benchmark PRIVATE cxx_std_23)
    target_link_libraries(sqlitecpp_benchmark PRIVATE SQLiteCpp::SQLiteCpp ${SQLITECPP_SQLITE_TARGET})

    set(SQLITECPP_REGRESSION_TESTS
        blob_example error_log error_log2 exception_dont_execute
        exception_dont_execute_nested exceptions extended_exceptions
        functions functors lvalue_functor_example mov_ctor nullptr_uniqueptr
        readme_example shared_connection simple_examples
        std_optional trycatchblocks variant)
    foreach(test_name IN LISTS SQLITECPP_REGRESSION_TESTS)
        add_executable(${test_name} tests/${test_name}.cc)
        target_compile_features(${test_name} PRIVATE cxx_std_23)
        target_link_libraries(${test_name} PRIVATE SQLiteCpp::SQLiteCpp ${SQLITECPP_SQLITE_TARGET})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
