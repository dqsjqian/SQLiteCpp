#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <expected>
#include <list>
#include <ranges>
#include <string>
#include <string_view>
#include <functional>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <utility>

#define SQLITE_CPP_VERSION_MAJOR 0
#define SQLITE_CPP_VERSION_MINOR 7
#define SQLITE_CPP_VERSION_PATCH 0

#include <sqlite3.h>

#include "SQLiteCpp/type_wrapper.h"
#include "SQLiteCpp/errors.h"
#include "SQLiteCpp/utility/function_traits.h"
#include "SQLiteCpp/utility/uncaught_exceptions.h"
#include "SQLiteCpp/utility/utf16_utf8.h"

namespace sqlite {

	class database;
	class database_binder;

	template<std::size_t> class binder;

	typedef std::shared_ptr<sqlite3> connection_type;

	// Any value the library knows how to bind into a statement and read back
	// from a result column (see the type_wrapper specialisations).
	template<typename T>
	concept bindable = is_sqlite_value<std::remove_cvref_t<T>>::value;

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

	namespace detail {
		// A prepared statement plus its reuse state, shared between the cache
		// and every binder that currently borrows it.
		struct statement_slot {
			std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt{nullptr, sqlite3_finalize};
			bool in_use = false;
		};

		// Bounded LRU cache of prepared statements.
		//
		// Statements are compiled once with SQLITE_PREPARE_PERSISTENT and then
		// reset + cleared on every reuse, which removes the per-execution
		// compile cost that dominates repetitive workloads. A statement that is
		// still executing (the same SQL re-entered from a row callback, say) is
		// never handed out twice: the caller gets an uncached statement instead,
		// so correctness never depends on cache state.
		class statement_cache {
		public:
			statement_cache(std::shared_ptr<sqlite3> db, std::size_t capacity):
				_db(std::move(db)), _capacity(capacity ? capacity : 1) {}

			std::shared_ptr<statement_slot> acquire(str_ref sql) {
				const std::string key(sql);
				auto found = _index.find(key);
				if(found != _index.end()) {
					if(!found->second->slot->in_use) {
						_lru.splice(_lru.begin(), _lru, found->second);
						found->second->slot->in_use = true;
						reset(found->second->slot);
						return found->second->slot;
					}
					return borrow_uncached(sql);
				}
				if(_index.size() < _capacity) {
					return store(key, sql);
				}
				// Full: reclaim the least recently used idle statement.
				for(auto it = _lru.end(); it != _lru.begin();) {
					--it;
					if(!it->slot->in_use) {
						_index.erase(it->key);
						_lru.erase(it);
						return store(key, sql);
					}
				}
				return borrow_uncached(sql);
			}

			// Called when the owning binder is done with the statement. Cached
			// slots stay resident; uncached ones are finalized when the last
			// owner drops them.
			void release(const std::shared_ptr<statement_slot>& slot) {
				if(!slot) return;
				slot->in_use = false;
				reset(slot);
			}

			std::size_t capacity() const { return _capacity; }
			std::size_t size() const { return _index.size(); }

		private:
			struct entry {
				std::string key;
				std::shared_ptr<statement_slot> slot;
			};

			static void reset(const std::shared_ptr<statement_slot>& slot) {
				if(slot->stmt) {
					sqlite3_reset(slot->stmt.get());
					sqlite3_clear_bindings(slot->stmt.get());
				}
			}

			std::shared_ptr<statement_slot> store(const std::string& key, str_ref sql) {
				auto slot = prepare(sql);
				_lru.push_front(entry{key, slot});
				_index.emplace(_lru.front().key, _lru.begin());
				slot->in_use = true;
				return slot;
			}

			std::shared_ptr<statement_slot> borrow_uncached(str_ref sql) {
				auto slot = prepare(sql);
				slot->in_use = true;
				return slot;
			}

			std::shared_ptr<statement_slot> prepare(str_ref sql) {
				sqlite3_stmt* raw = nullptr;
				const char* tail = nullptr;
				int rc = sqlite3_prepare_v3(_db.get(), sql.data(), static_cast<int>(sql.size()),
				                            SQLITE_PREPARE_PERSISTENT, &raw, &tail);
				if(rc != SQLITE_OK) {
#if SQLITE_VERSION_NUMBER >= 3038000
					const int offset = sqlite3_error_offset(_db.get());
#else
					const int offset = -1;
#endif
					errors::throw_sqlite_error(rc, sql, sqlite3_errmsg(_db.get()), offset);
				}
				if(raw && !std::all_of(tail, sql.data() + sql.size(),
				                       [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); })) {
					sqlite3_finalize(raw);
					throw errors::more_statements("Multiple semicolon separated statements are unsupported", sql);
				}
				auto slot = std::make_shared<statement_slot>();
				slot->stmt.reset(raw);
				return slot;
			}

			std::shared_ptr<sqlite3> _db;
			std::size_t _capacity;
			std::list<entry> _lru;                                            // front = most recently used
			std::unordered_map<std::string, std::list<entry>::iterator> _index;
		};
	}

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

	namespace sql_function_binder {
		template<
			typename    ContextType,
			std::size_t Count,
			typename    Functions
		>
		inline void step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals
		);

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) && sizeof...(Values) < Count), void>::type step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		);

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type step(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		);

		template<
			typename    ContextType,
			typename    Functions
		>
		inline void final(sqlite3_context* db);

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) < Count), void>::type scalar(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		);

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type scalar(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		);
	}

	enum class OpenFlags {
		READONLY = SQLITE_OPEN_READONLY,
		READWRITE = SQLITE_OPEN_READWRITE,
		CREATE = SQLITE_OPEN_CREATE,
		NOMUTEX = SQLITE_OPEN_NOMUTEX,
		FULLMUTEX = SQLITE_OPEN_FULLMUTEX,
		SHAREDCACHE = SQLITE_OPEN_SHAREDCACHE,
		PRIVATECACH = SQLITE_OPEN_PRIVATECACHE,
		URI = SQLITE_OPEN_URI
	};
	inline OpenFlags operator|(const OpenFlags& a, const OpenFlags& b) {
		return static_cast<OpenFlags>(static_cast<int>(a) | static_cast<int>(b));
	}
	enum class Encoding {
		ANY = SQLITE_ANY,
		UTF8 = SQLITE_UTF8,
		UTF16 = SQLITE_UTF16
	};
	struct sqlite_config {
		OpenFlags flags = OpenFlags::READWRITE | OpenFlags::CREATE;
		const char *zVfs = nullptr;
		Encoding encoding = Encoding::ANY;
		// Number of prepared statements kept alive for reuse. 0 disables reuse.
		std::size_t statement_cache_size = 32;
	};

	class database {
	protected:
		std::shared_ptr<sqlite3> _db;
		std::shared_ptr<detail::statement_cache> _statements;

	public:
		database(const std::string &db_name, const sqlite_config &config = {}): _db(nullptr) {
			sqlite3* tmp = nullptr;
			auto ret = sqlite3_open_v2(db_name.data(), &tmp, static_cast<int>(config.flags), config.zVfs);
			_db = std::shared_ptr<sqlite3>(tmp, [=](sqlite3* ptr) { sqlite3_close_v2(ptr); }); // this will close the connection eventually when no longer needed.
			if(ret != SQLITE_OK) errors::throw_sqlite_error(_db ? sqlite3_extended_errcode(_db.get()) : ret, {}, sqlite3_errmsg(_db.get()));
			sqlite3_extended_result_codes(_db.get(), true);
			_statements = std::make_shared<detail::statement_cache>(_db, config.statement_cache_size);
			if(config.encoding == Encoding::UTF16)
				*this << R"(PRAGMA encoding = "UTF-16";)";
		}

		database(const std::u16string &db_name, const sqlite_config &config = {}): database(utility::utf16_to_utf8(db_name), config) {
			if (config.encoding == Encoding::ANY)
				*this << R"(PRAGMA encoding = "UTF-16";)";
		}

		database(std::shared_ptr<sqlite3> db, std::size_t statement_cache_size = 32):
			_db(std::move(db)),
			_statements(std::make_shared<detail::statement_cache>(_db, statement_cache_size)) {}

		database_binder operator<<(str_ref sql) {
			return database_binder(_db, _statements, sql);
		}

		database_binder operator<<(u16str_ref sql) {
			return database_binder(_db, _statements, sql);
		}

		connection_type connection() const { return _db; }

		// Executes a parameterless statement and returns the affected row count.
		sqlite3_int64 execute(str_ref sql) {
			(*this << sql).execute();
			return rows_modified();
		}

		// Same as operator<< / execute, but reports failures through
		// std::expected instead of exceptions (noexcept), for -fno-exceptions
		// builds and callers that prefer value-based error handling.
		template<typename... Args>
			requires (bindable<Args> && ...)
		std::expected<sqlite3_int64, std::string> try_execute(str_ref sql, Args&&... args) noexcept {
			try {
				auto statement = *this << sql;
				((statement << std::forward<Args>(args)), ...);
				statement.execute();
				return rows_modified();
			} catch(const std::exception &error) {
				return std::unexpected(std::string(error.what()));
			}
		}

		// Binds one parameter tuple per range element and executes the same
		// prepared statement for every row: one compile, N executions.
		template<typename Range>
			requires std::ranges::input_range<Range> &&
			         detail::tuple_like<std::ranges::range_value_t<Range>>
		sqlite3_int64 execute_many(str_ref sql, const Range& rows) {
			auto slot = _statements->acquire(sql);
			sqlite3_stmt *stmt = slot->stmt.get();
			sqlite3_int64 executed = 0;
			try {
				if(stmt) {
					for(const auto &row : rows) {
						std::apply([stmt](const auto &...values) {
							int index = 1;
							(void)std::initializer_list<int>{
								(bind_col_in_db(stmt, index++, values), 0)...
							};
						}, row);
						if(int rc = sqlite3_step(stmt); rc != SQLITE_DONE)
							errors::throw_sqlite_error(rc, sql, sqlite3_errmsg(_db.get()));
						++executed;
						sqlite3_reset(stmt);
						sqlite3_clear_bindings(stmt);
					}
				}
			} catch(...) {
				_statements->release(slot);
				throw;
			}
			_statements->release(slot);
			return executed;
		}

		sqlite3_int64 last_insert_rowid() const {
			return sqlite3_last_insert_rowid(_db.get());
		}

		sqlite3_int64 rows_modified() const {
			return sqlite3_changes64(_db.get());
		}

		sqlite3_int64 total_changes() const {
			return sqlite3_total_changes64(_db.get());
		}

		template <typename Function>
		void define(const std::string &name, Function&& func) {
			typedef utility::function_traits<Function> traits;

			auto funcPtr = new auto(std::forward<Function>(func));
			if(int result = sqlite3_create_function_v2(
					_db.get(), name.data(), traits::arity, SQLITE_UTF8, funcPtr,
					sql_function_binder::scalar<traits::arity, typename std::remove_reference<Function>::type>,
					nullptr, nullptr, [](void* ptr){
				delete static_cast<decltype(funcPtr)>(ptr);
			}))
				errors::throw_sqlite_error(result, {}, sqlite3_errmsg(_db.get()));
		}

		// Registers a scalar SQL function with SQLite behaviour flags such as
		// deterministic / innocuous / direct_only.
		template <typename Function>
		void define(const std::string &name, function_flags flags, Function&& func) {
			typedef utility::function_traits<Function> traits;

			auto funcPtr = new auto(std::forward<Function>(func));
			if(int result = sqlite3_create_function_v2(
					_db.get(), name.data(), traits::arity,
					SQLITE_UTF8 | static_cast<int>(flags), funcPtr,
					sql_function_binder::scalar<traits::arity, typename std::remove_reference<Function>::type>,
					nullptr, nullptr, [](void* ptr){
				delete static_cast<decltype(funcPtr)>(ptr);
			}))
				errors::throw_sqlite_error(result, {}, sqlite3_errmsg(_db.get()));
		}

		template <typename StepFunction, typename FinalFunction>
		void define(const std::string &name, StepFunction&& step, FinalFunction&& final) {
			typedef utility::function_traits<StepFunction> traits;
			using ContextType = typename std::remove_reference<typename traits::template argument<0>>::type;

			auto funcPtr = new auto(std::make_pair(std::forward<StepFunction>(step), std::forward<FinalFunction>(final)));
			if(int result = sqlite3_create_function_v2(
					_db.get(), name.c_str(), traits::arity - 1, SQLITE_UTF8, funcPtr, nullptr,
					sql_function_binder::step<ContextType, traits::arity, typename std::remove_reference<decltype(*funcPtr)>::type>,
					sql_function_binder::final<ContextType, typename std::remove_reference<decltype(*funcPtr)>::type>,
					[](void* ptr){
				delete static_cast<decltype(funcPtr)>(ptr);
			}))
				errors::throw_sqlite_error(result, {}, sqlite3_errmsg(_db.get()));
		}

	};

	enum class transaction_behavior {
		deferred = 0,
		immediate = 1,
		exclusive = 2
	};

	// RAII transaction. Rolls back on destruction unless commit() succeeded.
	class transaction {
	public:
		explicit transaction(database &db, transaction_behavior behavior = transaction_behavior::deferred):
			_db(db.connection()), _behavior(behavior) {
			begin();
		}

		transaction(const transaction &) = delete;
		transaction &operator=(const transaction &) = delete;

		transaction(transaction &&other) noexcept:
			_db(std::move(other._db)), _behavior(other._behavior), _active(other._active) {
			other._active = false;
		}

		~transaction() noexcept {
			if(_active) {
				try {
					rollback();
				} catch(...) {
					// A destructor must not throw; the connection reports the
					// original failure on the next statement anyway.
				}
			}
		}

		bool active() const { return _active; }

		void commit() {
			end("COMMIT");
		}

		void rollback() {
			end("ROLLBACK");
		}

	private:
		void begin() {
			const char *sql = _behavior == transaction_behavior::immediate ? "BEGIN IMMEDIATE"
				: _behavior == transaction_behavior::exclusive ? "BEGIN EXCLUSIVE"
				: "BEGIN";
			execute(sql);
			_active = true;
		}

		void end(const char *sql) {
			if(!_active) throw std::logic_error("SQLiteCpp: transaction is no longer active");
			_active = false;
			execute(sql);
		}

		void execute(const char *sql) {
			char *errmsg = nullptr;
			int rc = sqlite3_exec(_db.get(), sql, nullptr, nullptr, &errmsg);
			if(rc != SQLITE_OK) {
				const std::string message = errmsg ? errmsg : sqlite3_errstr(rc);
				sqlite3_free(errmsg);
				errors::throw_sqlite_error(rc, sql, message.c_str());
			}
		}

		std::shared_ptr<sqlite3> _db;
		transaction_behavior _behavior;
		bool _active = false;
	};

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

	namespace sql_function_binder {
		template<class T>
		struct AggregateCtxt {
			T obj;
			bool constructed = true;
		};

		template<
			typename ContextType,
			std::size_t Count,
			typename    Functions
		>
		inline void step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals
		) {
			auto ctxt = static_cast<AggregateCtxt<ContextType>*>(sqlite3_aggregate_context(db, sizeof(AggregateCtxt<ContextType>)));
			if(!ctxt) return;
			try {
				if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
				step<Count, Functions>(db, count, vals, ctxt->obj);
				return;
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
			if(ctxt && ctxt->constructed)
				ctxt->~AggregateCtxt();
		}

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) && sizeof...(Values) < Count), void>::type step(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		) {
			using arg_type = typename std::remove_cv<
					typename std::remove_reference<
							typename utility::function_traits<
									typename Functions::first_type
							>::template argument<sizeof...(Values)>
					>::type
			>::type;

			step<Count, Functions>(
					db,
					count,
					vals,
					std::forward<Values>(values)...,
					get_val_from_db(vals[sizeof...(Values) - 1], result_type<arg_type>()));
		}

		template<
			std::size_t Count,
			typename    Functions,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type step(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		) {
			static_cast<Functions*>(sqlite3_user_data(db))->first(std::forward<Values>(values)...);
		}

		template<
			typename    ContextType,
			typename    Functions
		>
		inline void final(sqlite3_context* db) {
			auto ctxt = static_cast<AggregateCtxt<ContextType>*>(sqlite3_aggregate_context(db, sizeof(AggregateCtxt<ContextType>)));
			try {
				if(!ctxt) return;
				if(!ctxt->constructed) new(ctxt) AggregateCtxt<ContextType>();
				store_result_in_db(db,
						static_cast<Functions*>(sqlite3_user_data(db))->second(ctxt->obj));
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
			if(ctxt && ctxt->constructed)
				ctxt->~AggregateCtxt();
		}

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) < Count), void>::type scalar(
				sqlite3_context* db,
				int              count,
				sqlite3_value**  vals,
				Values&&...      values
		) {
			using arg_type = typename std::remove_cv<
					typename std::remove_reference<
							typename utility::function_traits<Function>::template argument<sizeof...(Values)>
					>::type
			>::type;

			scalar<Count, Function>(
					db,
					count,
					vals,
					std::forward<Values>(values)...,
					get_val_from_db(vals[sizeof...(Values)], result_type<arg_type>()));
		}

		template<
			std::size_t Count,
			typename    Function,
			typename... Values
		>
		inline typename std::enable_if<(sizeof...(Values) == Count), void>::type scalar(
				sqlite3_context* db,
				int,
				sqlite3_value**,
				Values&&...      values
		) {
			try {
				store_result_in_db(db,
						(*static_cast<Function*>(sqlite3_user_data(db)))(std::forward<Values>(values)...));
			} catch(const sqlite_exception &e) {
				sqlite3_result_error_code(db, e.get_code());
				sqlite3_result_error(db, e.what(), -1);
			} catch(const std::exception &e) {
				sqlite3_result_error(db, e.what(), -1);
			} catch(...) {
				sqlite3_result_error(db, "Unknown error", -1);
			}
		}
	}
}
