#pragma once

// Version of the library itself, independent of the SQLite version it wraps.
#define SQLITE_CPP_VERSION_MAJOR 1
#define SQLITE_CPP_VERSION_MINOR 2
#define SQLITE_CPP_VERSION_PATCH 0

// SQLiteCpp requires C++23.
//
// GCC 13 reports __cplusplus == 202100L for -std=c++23 because it implements
// the draft; GCC 14 and Clang report 202302L. MSVC and clang-cl report small
// _MSVC_LANG values even under /std:c++latest, so that branch only rejects
// pre-C++20 modes. Anything older fails on the first missing std facility.
#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 202004L
#    error "SQLiteCpp requires C++23 or newer (/std:c++latest)"
#  endif
#elif !defined(__cplusplus) || __cplusplus < 202100L
#  error "SQLiteCpp requires C++23 or newer (-std=c++23)"
#endif
