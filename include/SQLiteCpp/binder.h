#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <sqlite3.h>
#include "fwd.h"
#include "types.h"
#include "errors.h"
#include "detail/statement_cache.h"
#include "detail/value_traits.h"
#include "detail/function_traits.h"
#include "detail/utf.h"
#include "detail/uncaught_exceptions.h"

namespace sqlite {

	class row_iterator;
	class database_binder {

	public:
		// database_binder is not copyable
		database_binder() = delete;
		database_binder(const database_binder& other) = delete;
		database_binder& operator=(const database_binder&) = delete;

		database_binder(database_binder&& other) :
			_db(std::move(other._db)),
			_cache(std::move(other._cache)),
			_slot(std::move(other._slot)),
			_inx(other._inx), execution_started(other.execution_started) {
			other._slot.reset();
		}

		void execute();

		std::string sql() {
			auto sqlite_deleter = [](void *ptr) {sqlite3_free(ptr);};
			std::unique_ptr<char, decltype(sqlite_deleter)> str(sqlite3_expanded_sql(_stmt()), sqlite_deleter);
			return str ? str.get() : original_sql();
		}

		std::string original_sql() {
			const char *sql = sqlite3_sql(_stmt());
			return sql ? sql : std::string();
		}

		void used(bool state) {
			if(!state) {
				// We may have to reset first if we haven't done so already:
				_next_index();
				--_inx;
			}
			execution_started = state;
		}
		bool used() const { return execution_started; }
		row_iterator begin();
		row_iterator end();

	private:
		std::shared_ptr<sqlite3> _db;
		std::shared_ptr<detail::statement_cache> _cache;
		std::shared_ptr<detail::statement_slot> _slot;
		utility::UncaughtExceptionDetector _has_uncaught_exception;

		int _inx = 0;

		bool execution_started = false;

		sqlite3_stmt *_stmt() const { return _slot ? _slot->stmt.get() : nullptr; }

		int _next_index() {
			if(execution_started && !_inx) {
				sqlite3_reset(_stmt());
				sqlite3_clear_bindings(_stmt());
			}
			return ++_inx;
		}

		template<typename T> friend database_binder& operator<<(database_binder& db, T&&);
		template<typename T> friend database_binder& operator<<(database_binder& db, index_binding_helper<T>);
		template<typename T> friend database_binder& operator<<(database_binder& db, index_binding_helper<T, true>);
		friend void operator++(database_binder& db, int);

	public:

		database_binder(std::shared_ptr<sqlite3> db, std::shared_ptr<detail::statement_cache> cache, str_ref sql):
			_db(std::move(db)),
			_cache(std::move(cache)),
			_inx(0) {
			_slot = _cache->acquire(sql);
		}

		database_binder(std::shared_ptr<sqlite3> db, std::shared_ptr<detail::statement_cache> cache, u16str_ref sql):
			_db(std::move(db)),
			_cache(std::move(cache)),
			_inx(0) {
			const std::string utf8 = utility::utf16_to_utf8(sql);
			_slot = _cache->acquire(utf8);
		}

		~database_binder() noexcept(false) {
			/* Will be executed if no >>op is found, but not if an exception
			is in mid flight */
			if(!used() && !_has_uncaught_exception && _slot) {
				execute();
			}
			if(_slot) _cache->release(_slot);
		}

		friend class row_iterator;
	};


	class row_iterator {
	public:
		class value_type {
		public:
			value_type(database_binder *_binder): _binder(_binder) {};
			template<class T>
			typename std::enable_if<is_sqlite_value<T>::value, value_type &>::type operator >>(T &result) {
				result = get_col_from_db(_binder->_stmt(), next_index++, result_type<T>());
				return *this;
			}
			template<class ...Types>
			value_type &operator >>(std::tuple<Types...>& values) {
				values = handle_tuple<std::tuple<typename std::decay<Types>::type...>>(std::index_sequence_for<Types...>());
				next_index += sizeof...(Types);
				return *this;
			}
			template<class ...Types>
			value_type &operator >>(std::tuple<Types...>&& values) {
				return *this >> values;
			}
			template<class ...Types>
			operator std::tuple<Types...>() {
				std::tuple<Types...> value;
				*this >> value;
				return value;
			}
			// Reads the next columns as the given types, enabling structured
			// bindings: auto [id, name] = row.as<int, std::string>();
			template<typename... Types>
			std::tuple<Types...> as() {
				std::tuple<Types...> values;
				*this >> values;
				return values;
			}
			explicit operator bool() {
				return sqlite3_column_count(_binder->_stmt()) >= next_index;
			}
		private:
			template<class Tuple, std::size_t ...Index>
			Tuple handle_tuple(std::index_sequence<Index...>) {
				return Tuple(
						get_col_from_db(
							_binder->_stmt(),
							next_index + Index,
							result_type<typename std::tuple_element<Index, Tuple>::type>())...);
			}
			database_binder *_binder;
			int next_index = 0;
		};
		using difference_type = std::ptrdiff_t;
		using pointer = value_type*;
		using reference = value_type&;
		using iterator_category = std::input_iterator_tag;

		row_iterator() = default;
		explicit row_iterator(database_binder &binder): _binder(&binder) {
			_binder->_next_index();
			_binder->_inx = 0;
			_binder->used(true);
			++*this;
		}

		reference operator*() const { return value;}
		pointer operator->() const { return std::addressof(**this); }
		row_iterator &operator++() {
			switch(int result = sqlite3_step(_binder->_stmt())) {
				case SQLITE_ROW:
					value = {_binder};
					break;
				case SQLITE_DONE:
					_binder = nullptr;
					break;
				default:
					exceptions::throw_sqlite_error(result, _binder->sql(), sqlite3_errmsg(_binder->_db.get()));
			}
			return *this;
		}

		friend inline bool operator ==(const row_iterator &a, const row_iterator &b) {
			return a._binder == b._binder;
		}
		friend inline bool operator !=(const row_iterator &a, const row_iterator &b) {
			return !(a==b);
		}

	private:
		database_binder *_binder = nullptr;
		mutable value_type value{_binder}; // mutable, because `changing` the value is just reading it
	};

	inline row_iterator database_binder::begin() {
		return row_iterator(*this);
	}

	inline row_iterator database_binder::end() {
		return row_iterator();
	}

	namespace detail {
		template<class Callback>
		void _extract_single_value(database_binder &binder, Callback call_back) {
			auto iter = binder.begin();
			if(iter == binder.end())
				throw errors::no_rows("no rows to extract: exactly 1 row expected", binder.sql(), SQLITE_DONE);

			call_back(*iter);

			if(++iter != binder.end())
				throw errors::more_rows("not all rows extracted", binder.sql(), SQLITE_ROW);
		}
	}
	inline void database_binder::execute() {
		for(auto &&row : *this)
			(void)row;
	}
	namespace detail {
		template<class T> using void_t = void;
		template<class T, class = void>
		struct sqlite_direct_result : std::false_type {};
		template<class T>
		struct sqlite_direct_result<
			T,
			void_t<decltype(std::declval<row_iterator::value_type&>() >> std::declval<T&&>())>
		> : std::true_type {};
	}
	template <typename Result>
	inline typename std::enable_if<detail::sqlite_direct_result<Result>::value>::type operator>>(database_binder &binder, Result&& value) {
		detail::_extract_single_value(binder, [&value] (row_iterator::value_type &row) {
			row >> std::forward<Result>(value);
		});
	}

	template <typename Function>
	inline typename std::enable_if<!detail::sqlite_direct_result<Function>::value>::type operator>>(database_binder &db_binder, Function&& func) {
		using traits = utility::function_traits<Function>;

		for(auto &&row : db_binder) {
			binder<traits::arity>::run(row, func);
		}
	}

	template <typename Result>
	inline decltype(auto) operator>>(database_binder &&binder, Result&& value) {
		return binder >> std::forward<Result>(value);
	}

	template<std::size_t Count>
	class binder {
	private:
		template <
			typename    Function,
			std::size_t Index
		>
			using nth_argument_type = typename utility::function_traits<
			Function
			>::template argument<Index>;

	public:
		// `Boundary` needs to be defaulted to `Count` so that the `run` function
		// template is not implicitly instantiated on class template instantiation.
		// Look up section 14.7.1 _Implicit instantiation_ of the ISO C++14 Standard

		template<
			typename    Function,
			typename... Values,
			std::size_t Boundary = Count
		>
			static typename std::enable_if<(sizeof...(Values) < Boundary), void>::type run(
				row_iterator::value_type& row,
				Function&&                function,
				Values&&...               values
				) {
			typename std::decay<nth_argument_type<Function, sizeof...(Values)>>::type value;
			row >> value;
			run<Function>(row, function, std::forward<Values>(values)..., std::move(value));
		}

		template<
			typename    Function,
			typename... Values,
			std::size_t Boundary = Count
		>
			static typename std::enable_if<(sizeof...(Values) == Boundary), void>::type run(
				row_iterator::value_type&,
				Function&&                function,
				Values&&...               values
				) {
			function(std::move(values)...);
		}
	};

	// Some ppl are lazy so we have a operator for proper prep. statemant handling.
	void inline operator++(database_binder& db, int) { db.execute(); }

	template<typename T> database_binder &operator<<(database_binder& db, index_binding_helper<T> val) {
		db._next_index(); --db._inx;
		int result = bind_col_in_db(db._stmt(), val.index, std::forward<T>(val.value));
		if(result != SQLITE_OK)
			exceptions::throw_sqlite_error(result, db.sql(), sqlite3_errmsg(db._db.get()));
		return db;
	}

	template<typename T> database_binder &operator<<(database_binder& db, index_binding_helper<T, true> val) {
		db._next_index(); --db._inx;
		int index = sqlite3_bind_parameter_index(db._stmt(), val.index);
		if(!index)
			throw errors::unknown_binding("The given binding name is not valid for this statement", db.sql());
		int result = bind_col_in_db(db._stmt(), index, std::forward<T>(val.value));
		if(result != SQLITE_OK)
			exceptions::throw_sqlite_error(result, db.sql(), sqlite3_errmsg(db._db.get()));
		return db;
	}

	template<typename T> database_binder &operator<<(database_binder& db, T&& val) {
		int result = bind_col_in_db(db._stmt(), db._next_index(), std::forward<T>(val));
		if(result != SQLITE_OK)
			exceptions::throw_sqlite_error(result, db.sql(), sqlite3_errmsg(db._db.get()));
		return db;
	}
	// Convert the rValue binder to a reference and call first op<<, its needed for the call that creates the binder (be carefull of recursion here!)
	template<typename T> database_binder operator << (database_binder&& db, const T& val) { db << val; return std::move(db); }
	template<typename T, bool Name> database_binder operator << (database_binder&& db, index_binding_helper<T, Name> val) { db << index_binding_helper<T, Name>{val.index, std::forward<T>(val.value)}; return std::move(db); }

} // namespace sqlite
