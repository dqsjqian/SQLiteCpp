#pragma once

// Version of the library itself, independent of the SQLite version it wraps.
#define SQLITE_CPP_VERSION_MAJOR 0
#define SQLITE_CPP_VERSION_MINOR 7
#define SQLITE_CPP_VERSION_PATCH 0

// SQLiteCpp requires C++23.
#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 202302L
#    error "SQLiteCpp requires C++23 or newer (/std:c++latest)"
#  endif
#elif __cplusplus < 202302L
#  error "SQLiteCpp requires C++23 or newer (-std=c++23)"
#endif
