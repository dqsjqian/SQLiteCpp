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

		database_binder(database_binder&& other) noexcept :
			_cache(std::move(other._cache)),
			_slot(other._slot),
			_owned(std::move(other._owned)),
			_has_uncaught_exception(other._has_uncaught_exception),
			_inx(other._inx), execution_started(other.execution_started),
			_stepped(other._stepped), _reset_done(other._reset_done),
			_neutralised(other._neutralised) {
			other._slot = nullptr;
		}

		void execute();

		std::string sql() {
			auto sqlite_deleter = [](void *ptr) {sqlite3_free(ptr);};
			std::unique_ptr<char, decltype(sqlite_deleter)> str(sqlite3_expanded_sql(_stmt()), sqlite_deleter);
			return str ? str.get() : original_sql();
		}

		// ---- result column metadata ----------------------------------------
		//
		// Valid once the statement is prepared; the table/origin accessors need
		// SQLITE_ENABLE_COLUMN_METADATA when SQLite itself is built.

		int column_count() const {
			return sqlite3_column_count(_stmt());
		}

		std::string column_name(int index) const {
			const char *name = sqlite3_column_name(_stmt(), index);
			return name ? name : std::string();
		}

		std::string column_declared_type(int index) const {
			const char *type = sqlite3_column_decltype(_stmt(), index);
			return type ? type : std::string();
		}

#ifdef SQLITE_ENABLE_COLUMN_METADATA
		std::string column_table_name(int index) const {
			const char *name = sqlite3_column_table_name(_stmt(), index);
			return name ? name : std::string();
		}

		std::string column_origin_name(int index) const {
			const char *name = sqlite3_column_origin_name(_stmt(), index);
			return name ? name : std::string();
		}
#endif

		std::string original_sql() {
			const char *sql = sqlite3_sql(_stmt());
			return sql ? sql : std::string();
		}

		// used(false) marks the binder as not executed, so the destructor runs
		// it again with the bindings of the last round; used(true) suppresses
		// the implicit execution.
		void used(bool state) {
			if(!state && _stepped) {
				if(!_reset_done) sqlite3_reset(_stmt());
				_reset_done = true;
				_stepped = false;
				_inx = 0;
				_neutralised = true; // keep the last round's values
			}
			execution_started = state;
		}
		bool used() const { return execution_started; }
		row_iterator begin();
		row_iterator end();

	private:
		std::shared_ptr<detail::statement_cache> _cache;
		// Borrowed from the cache, or owned through _owned when the cache did
		// not retain it. A raw pointer avoids reference counting per execution.
		detail::statement_slot *_slot = nullptr;
		std::unique_ptr<detail::statement_slot> _owned;
		utility::UncaughtExceptionDetector _has_uncaught_exception;

		// A "round" is: bind parameters, then step. The statement may come from
		// the cache with bindings left by an earlier round, so every round makes
		// sure parameters it did not bind read as NULL, exactly as on a freshly
		// prepared statement.
		int _inx = 0;                   // sequential parameter cursor of this round
		bool execution_started = false; // used(): executed at least once
		bool _stepped = false;          // this round has been stepped
		bool _reset_done = true;        // nothing to reset on release
		bool _neutralised = false;      // unbound parameters of this round are NULL

		sqlite3_stmt *_stmt() const { return _slot ? _slot->stmt.get() : nullptr; }

		// A bind after the round was stepped opens a new round.
		void _begin_bind() {
			if(!_stepped) return;
			if(!_reset_done) sqlite3_reset(_stmt());
			_reset_done = true;
			_stepped = false;
			_inx = 0;
			_neutralised = false;
		}

		// Binds NULL to every parameter after `last`. When the round binds every
		// parameter in order this loop runs zero times.
		void _neutralise_after(int last) {
			const int count = sqlite3_bind_parameter_count(_stmt());
			for(int index = last + 1; index <= count; ++index)
				sqlite3_bind_null(_stmt(), index);
			_neutralised = true;
		}

		int _next_index() {
			_begin_bind();
			return ++_inx;
		}

		// Positional or named binds can land anywhere, so stale values are
		// cleared before the first one instead of at the first step.
		void _prepare_explicit_bind() {
			_begin_bind();
			if(!_neutralised) _neutralise_after(_inx);
		}

		// Runs right before the first step. Re-running a round that already
		// stepped keeps its bindings, like sqlite3_reset does.
		void _begin_step() {
			if(_stepped) {
				if(!_reset_done) sqlite3_reset(_stmt());
			} else if(!_neutralised) {
				_neutralise_after(_inx);
			}
			_stepped = true;
			_reset_done = false;
			// Marked before stepping: if the step throws, the destructor must not
			// run the statement a second time.
			execution_started = true;
		}

		template<typename T> friend database_binder& operator<<(database_binder& db, T&&);
		template<typename T> friend database_binder& operator<<(database_binder& db, index_binding_helper<T>);
		template<typename T> friend database_binder& operator<<(database_binder& db, index_binding_helper<T, true>);
		friend void operator++(database_binder& db, int);

	public:

		database_binder(std::shared_ptr<detail::statement_cache> cache, str_ref sql):
			_cache(std::move(cache)),
			_inx(0) {
			auto borrowed = _cache->acquire(sql);
			_slot = borrowed.first;
			if(!borrowed.second) _owned.reset(_slot);
		}

		database_binder(std::shared_ptr<detail::statement_cache> cache, u16str_ref sql):
			_cache(std::move(cache)),
			_inx(0) {
			const std::string utf8 = utility::utf16_to_utf8(sql);
			auto borrowed = _cache->acquire(utf8);
			_slot = borrowed.first;
			if(!borrowed.second) _owned.reset(_slot);
		}

		// Throwing is the contract here: a statement that was never consumed
		// runs at the end of the full expression and reports its failure. It
		// does not throw while another exception is in flight.
		~database_binder() noexcept(false) { // NOLINT(bugprone-unsafe-to-allow-exceptions)
			// Runs a statement nobody consumed, unless an exception is in
			// flight. The cheap checks go first: every `<<` on a temporary
			// leaves a moved-from binder behind, and those must cost nothing.
			if(_slot && !used() && !_has_uncaught_exception) {
				execute();
			}
			if(_slot && !_owned) _cache->release(_slot, !_reset_done);
			_owned.reset();
		}

		friend class row_iterator;
	};


	class row_iterator {
	public:
		class value_type {
		public:
			value_type(database_binder *binder): _binder(binder) {}
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
				std::tuple<Types...> values;
				*this >> values;
				return values;
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
			if(_binder->_stmt()) _binder->_begin_step();
			else _binder->execution_started = true;
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
					exceptions::throw_sqlite_error(result, _binder->sql(), sqlite3_errmsg(_binder->_cache->connection()));
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
		if(!_stmt()) {
			execution_started = true;
			return;
		}
		// Step directly instead of materialising the row iterator: same
		// semantics, less per-row work on the hot path.
		_begin_step();
		while(true) {
			const int rc = sqlite3_step(_stmt());
			if(rc == SQLITE_ROW) continue;
			if(rc == SQLITE_DONE) break;
			errors::throw_sqlite_error(rc, sql(), sqlite3_errmsg(_cache->connection()));
		}
		// Reset now so locks are released and the binder can be rebound; the
		// cache then has nothing left to do on release.
		sqlite3_reset(_stmt());
		_reset_done = true;
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
		db._prepare_explicit_bind();
		int result = bind_col_in_db(db._stmt(), val.index, std::forward<T>(val.value));
		if(result != SQLITE_OK)
			exceptions::throw_sqlite_error(result, db.sql(), sqlite3_errmsg(db._cache->connection()));
		return db;
	}

	template<typename T> database_binder &operator<<(database_binder& db, index_binding_helper<T, true> val) {
		db._prepare_explicit_bind();
		int index = sqlite3_bind_parameter_index(db._stmt(), val.index);
		if(!index)
			throw errors::unknown_binding("The given binding name is not valid for this statement", db.sql());
		int result = bind_col_in_db(db._stmt(), index, std::forward<T>(val.value));
		if(result != SQLITE_OK)
			exceptions::throw_sqlite_error(result, db.sql(), sqlite3_errmsg(db._cache->connection()));
		return db;
	}

	template<typename T> database_binder &operator<<(database_binder& db, T&& val) {
		int result = bind_col_in_db(db._stmt(), db._next_index(), std::forward<T>(val));
		if(result != SQLITE_OK)
			exceptions::throw_sqlite_error(result, db.sql(), sqlite3_errmsg(db._cache->connection()));
		return db;
	}
	// Convert the rValue binder to a reference and call first op<<, its needed for the call that creates the binder (be carefull of recursion here!)
	template<typename T> database_binder operator << (database_binder&& db, const T& val) { db << val; return std::move(db); }
	template<typename T, bool Name> database_binder operator << (database_binder&& db, index_binding_helper<T, Name> val) { db << index_binding_helper<T, Name>{val.index, std::forward<T>(val.value)}; return std::move(db); }

} // namespace sqlite
