#pragma once

#include <concepts>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <sqlite3.h>

// SQLiteCpp targets one SQLite line: the newest. 3.38 is the floor because
// sqlite3_error_offset() is used unconditionally; the repository vendors
// 3.54.1 in vendor/sqlite, and consumers are expected to link that or newer.
#if !defined(SQLITE_VERSION_NUMBER) || SQLITE_VERSION_NUMBER < 3038000
#  error "SQLiteCpp requires SQLite 3.38 or newer (vendor/sqlite ships 3.54.1)"
#endif
#include "types.h"

namespace sqlite {

	class database;
	class database_binder;

	template<std::size_t> class binder;

	typedef std::shared_ptr<sqlite3> connection_type;

	// Outcome of a statement executed through database::try_execute, which
	// reports failures by value instead of by exception. An empty `error`
	// means success.
	struct execution_result {
		sqlite3_int64 changes = 0;
		int code = SQLITE_OK;
		std::string error;

		explicit operator bool() const { return error.empty(); }
		bool has_value() const { return error.empty(); }
		sqlite3_int64 value() const { return changes; }
	};

	// Flags for database::define(), mirroring the SQLite function flags.
	enum class function_flags : int {
		none = 0,
		deterministic = SQLITE_DETERMINISTIC,
		innocuous = SQLITE_INNOCUOUS,
		direct_only = SQLITE_DIRECTONLY,
		subtype = SQLITE_SUBTYPE
	};
	constexpr function_flags operator|(function_flags a, function_flags b) {
		return static_cast<function_flags>(static_cast<int>(a) | static_cast<int>(b));
	}

	namespace detail {
		template<typename T>
		concept tuple_like = requires { typename std::tuple_size<std::remove_cvref_t<T>>::type; };
	}

	template<class T, bool Name = false>
	struct index_binding_helper {
		typedef typename std::conditional<Name, const char *, int>::type index_type;

		index_binding_helper(const index_binding_helper &) = delete;
		// C++20 stopped treating a class with a user-declared constructor as an
		// aggregate, so the call sites below cannot use brace initialization of
		// the members alone. This constructor keeps the same two-argument
		// spelling on every supported standard without allowing copies.
		index_binding_helper(index_type index_, T &&value_)
			: index(index_), value(std::forward<T>(value_)) {}

		index_type index;
		T value;
	};

	template<class T>
	auto named_parameter(const char *name, T &&arg) {
		return index_binding_helper<decltype(arg), true>{name, std::forward<decltype(arg)>(arg)};
	}
	template<class T>
	auto indexed_parameter(int index, T &&arg) {
		return index_binding_helper<decltype(arg)>{index, std::forward<decltype(arg)>(arg)};
	}

} // namespace sqlite
