#pragma once

// Version of the library itself, independent of the SQLite version it wraps.
#define SQLITE_CPP_VERSION_MAJOR 1
#define SQLITE_CPP_VERSION_MINOR 0
#define SQLITE_CPP_VERSION_PATCH 0

// SQLiteCpp requires C++23.
//
// GCC 13 reports __cplusplus == 202100L for -std=c++23 because it implements
// the draft; GCC 14 and Clang report 202302L. Accept both spellings and then
// gate on the library feature the headers actually require.
#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 202302L
#    error "SQLiteCpp requires C++23 or newer (/std:c++latest)"
#  endif
#elif !defined(__cplusplus) || __cplusplus < 202100L
#  error "SQLiteCpp requires C++23 or newer (-std=c++23)"
#endif

#if __has_include(<version>)
#  include <version>
#endif

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#  error "SQLiteCpp requires C++23 library support (std::expected)"
#endif
