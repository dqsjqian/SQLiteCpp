#pragma once

#include <concepts>
#include <cstring>
#include <functional>
#include <span>
#include <vector>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <sqlite3.h>
#include "fwd.h"
#include "types.h"
#include "errors.h"
#include "binder.h"
#include "sql_function.h"
#include "detail/statement_cache.h"
#include "detail/value_traits.h"
#include "detail/utf.h"

namespace sqlite {

	namespace detail {
		template<typename Record>
		inline void read_members(row_iterator::value_type &, Record &) {}

		template<typename Record, typename Member, typename... Rest>
		void read_members(row_iterator::value_type &row, Record &record,
		                  Member Record::*member, Rest Record::*...rest) {
			row >> (record.*member);
			read_members(row, record, rest...);
		}
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
		// Milliseconds to wait for a locked table before reporting SQLITE_BUSY.
		int busy_timeout_ms = 0;
	};

	// Connection-level toggles exposed through sqlite3_db_config.
	enum class config_flag : int {
		foreign_keys = SQLITE_DBCONFIG_ENABLE_FKEY,
		triggers = SQLITE_DBCONFIG_ENABLE_TRIGGER,
		views = SQLITE_DBCONFIG_ENABLE_VIEW,
		defensive = SQLITE_DBCONFIG_DEFENSIVE,
		trusted_schema = SQLITE_DBCONFIG_TRUSTED_SCHEMA,
		writable_schema = SQLITE_DBCONFIG_WRITABLE_SCHEMA,
		load_extension = SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,
		scanstatus = SQLITE_DBCONFIG_STMT_SCANSTATUS
	};

	class database {
	protected:
		// Per-connection callbacks. Kept alive by the connection deleter so a
		// registered hook never outlives its context.
		struct connection_state {
			std::function<int()> progress;
			// Views are valid for the duration of the call only.
			std::function<void(int action, std::string_view database, std::string_view table,
			                   sqlite3_int64 rowid)> update;
			std::function<int()> commit;
			std::function<void()> rollback;
		};

		std::shared_ptr<sqlite3> _db;
		std::shared_ptr<detail::statement_cache> _statements;
		std::shared_ptr<connection_state> _state;

	public:
		database(const std::string &db_name, const sqlite_config &config = {}): _db(nullptr) {
			sqlite3* tmp = nullptr;
			auto ret = sqlite3_open_v2(db_name.data(), &tmp, static_cast<int>(config.flags), config.zVfs);
			_state = std::make_shared<connection_state>();
			// The deleter keeps the progress state alive exactly as long as the
			// connection, so a registered handler never outlives its context.
			auto state_keepalive = _state;
			_db = std::shared_ptr<sqlite3>(tmp, [state_keepalive](sqlite3* ptr) {
				(void)state_keepalive;
				sqlite3_close_v2(ptr);
			});
			if(ret != SQLITE_OK) errors::throw_sqlite_error(_db ? sqlite3_extended_errcode(_db.get()) : ret, {}, sqlite3_errmsg(_db.get()));
			sqlite3_extended_result_codes(_db.get(), true);
			if(config.busy_timeout_ms > 0) sqlite3_busy_timeout(_db.get(), config.busy_timeout_ms);
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
			_statements(std::make_shared<detail::statement_cache>(_db, statement_cache_size)),
			// Callbacks registered on this connection live at most as long as
			// this database object; a shared raw handle cannot extend them.
			_state(std::make_shared<connection_state>()) {}

		database_binder operator<<(str_ref sql) {
			return database_binder(_statements, sql);
		}

		database_binder operator<<(u16str_ref sql) {
			return database_binder(_statements, sql);
		}

		connection_type connection() const { return _db; }

		// Waits up to `milliseconds` for a locked table instead of reporting
		// SQLITE_BUSY immediately. Essential for multi-threaded writers.
		void busy_timeout(int milliseconds) {
			if(int rc = sqlite3_busy_timeout(_db.get(), milliseconds); rc != SQLITE_OK)
				errors::throw_sqlite_error(rc, {}, sqlite3_errmsg(_db.get()));
		}

		// Toggles a connection-level behaviour (sqlite3_db_config), e.g.
		// configure(config_flag::foreign_keys, true).
		void configure(config_flag flag, bool enable) {
			int current = 0;
			if(int rc = sqlite3_db_config(_db.get(), static_cast<int>(flag), enable ? 1 : 0, &current);
			   rc != SQLITE_OK)
				errors::throw_sqlite_error(rc, {}, sqlite3_errmsg(_db.get()));
		}

		// Reads back a connection-level toggle: -1 when it cannot be queried.
		bool configured(config_flag flag) {
			int current = 0;
			if(sqlite3_db_config(_db.get(), static_cast<int>(flag), -1, &current) != SQLITE_OK)
				return false;
			return current != 0;
		}

		// Aborts any statement currently running on this connection from
		// another thread (sqlite3_interrupt).
		void interrupt() {
			sqlite3_interrupt(_db.get());
		}

		// Installs a progress callback invoked every `instructions` virtual
		// machine instructions. Returning non-zero aborts the running
		// statement with SQLITE_INTERRUPT.
		void progress_handler(std::function<int()> handler, int instructions = 1000) {
			if(!_state) return;
			_state->progress = std::move(handler);
			if(_state->progress)
				sqlite3_progress_handler(_db.get(), instructions,
				                         &database::progress_trampoline, _state.get());
			else
				sqlite3_progress_handler(_db.get(), 0, nullptr, nullptr);
		}

		void clear_progress_handler() {
			progress_handler({}, 0);
		}

		// Called after INSERT / UPDATE / DELETE. action is SQLITE_INSERT,
		// SQLITE_UPDATE or SQLITE_DELETE. The database and table names are views
		// into SQLite memory, valid only during the call: copy them to keep
		// them. An exception thrown by the handler is swallowed.
		//
		// Handlers written for 1.1.0, taking std::string names, keep working.
		template<typename Handler>
			requires std::invocable<Handler &, int, std::string_view, std::string_view, sqlite3_int64>
			      || std::invocable<Handler &, int, std::string, std::string, sqlite3_int64>
		void on_update(Handler &&handler) {
			if constexpr(std::is_constructible_v<bool, Handler &>) {
				if(!static_cast<bool>(handler)) {
					set_update_hook({});
					return;
				}
			}
			if constexpr(std::invocable<Handler &, int, std::string_view, std::string_view, sqlite3_int64>) {
				set_update_hook(std::forward<Handler>(handler));
			} else {
				set_update_hook([inner = std::forward<Handler>(handler)](
						int action, std::string_view database_name, std::string_view table_name,
						sqlite3_int64 rowid) mutable {
					inner(action, std::string(database_name), std::string(table_name), rowid);
				});
			}
		}

		// Removes the update hook: on_update(nullptr).
		void on_update(std::nullptr_t) { set_update_hook({}); }

	private:
		void set_update_hook(std::function<void(int, std::string_view, std::string_view, sqlite3_int64)> handler) {
			if(!_state) return;
			_state->update = std::move(handler);
			sqlite3_update_hook(_db.get(), _state->update ? &database::update_trampoline : nullptr,
			                    _state->update ? _state.get() : nullptr);
		}

	public:

		// Returning non-zero from the handler turns the commit into a rollback.
		void on_commit(std::function<int()> handler) {
			if(!_state) return;
			_state->commit = std::move(handler);
			sqlite3_commit_hook(_db.get(), _state->commit ? &database::commit_trampoline : nullptr,
			                    _state->commit ? _state.get() : nullptr);
		}

		void on_rollback(std::function<void()> handler) {
			if(!_state) return;
			_state->rollback = std::move(handler);
			sqlite3_rollback_hook(_db.get(), _state->rollback ? &database::rollback_trampoline : nullptr,
			                      _state->rollback ? _state.get() : nullptr);
		}

		// Query plan lines for a statement (EXPLAIN QUERY PLAN).
		std::vector<std::string> explain(str_ref sql) {
			std::vector<std::string> plan;
			auto statement = *this << ("EXPLAIN QUERY PLAN " + std::string(sql));
			for(auto &&row : statement) {
				int id = 0;
				int parent = 0;
				int not_used = 0;
				std::string detail;
				row >> id >> parent >> not_used >> detail;
				plan.push_back(detail);
			}
			return plan;
		}

		// Loads a SQLite extension (sqlite3_load_extension). Extension loading
		// must be enabled first: configure(config_flag::load_extension, true).
		void load_extension(str_ref file, str_ref entry = {}) {
			const std::string path(file);
			const std::string symbol(entry);
			char *error = nullptr;
			const int rc = sqlite3_load_extension(_db.get(), path.c_str(),
			                                      entry.empty() ? nullptr : symbol.c_str(), &error);
			if(rc != SQLITE_OK) {
				const std::string message = error ? error : sqlite3_errmsg(_db.get());
				sqlite3_free(error);
				errors::throw_sqlite_error(rc, "load_extension", message.c_str());
			}
		}

		// Bytes currently used by this connection's pager cache.
		sqlite3_int64 cache_used() const {
			int current = 0;
			int highwater = 0;
			if(sqlite3_db_status(_db.get(), SQLITE_DBSTATUS_CACHE_USED, &current, &highwater, 0)
			   != SQLITE_OK)
				return 0;
			return current;
		}

		// Bytes currently held by this connection's schema cache.
		sqlite3_int64 schema_used() const {
			int current = 0;
			int highwater = 0;
			if(sqlite3_db_status(_db.get(), SQLITE_DBSTATUS_SCHEMA_USED, &current, &highwater, 0)
			   != SQLITE_OK)
				return 0;
			return current;
		}

		// Reads every row into a Record, assigning the columns to the given
		// member pointers in order.
		template<typename Record, typename... Members>
		std::vector<Record> select(str_ref sql, Members Record::*...members) {
			std::vector<Record> rows_out;
			auto statement = *this << sql;
			for(auto &&row : statement) {
				Record record{};
				detail::read_members(row, record, members...);
				rows_out.push_back(std::move(record));
			}
			return rows_out;
		}

		// Executes a pragma (or any single-column query) and returns the first
		// column of the first row as text, e.g. pragma("PRAGMA user_version").
		std::string pragma(str_ref sql) {
			std::string value;
			auto statement = *this << sql;
			for(auto &&row : statement) {
				row >> value;
				break;
			}
			return value;
		}

		// Switches the journal mode to WAL (or back to DELETE). Reports whether
		// the requested mode is active afterwards.
		bool wal(bool enable = true) {
			return pragma(enable ? "PRAGMA journal_mode=WAL" : "PRAGMA journal_mode=DELETE")
				== (enable ? "wal" : "delete");
		}

		// Byte-for-byte image of the database.
		std::vector<unsigned char> serialize(str_ref schema = "main") {
			sqlite3_int64 size = 0;
			const std::string schema_name(schema); // NUL-terminated for SQLite
			unsigned char *data = sqlite3_serialize(_db.get(), schema_name.c_str(), &size, 0);
			if(!data || size <= 0) {
				sqlite3_free(data);
				return {};
			}
			std::vector<unsigned char> image(data, data + size);
			sqlite3_free(data);
			return image;
		}

		// Replaces the database image with the given bytes.
		void deserialize(std::span<const unsigned char> image, str_ref schema = "main") {
			const std::string schema_name(schema); // NUL-terminated for SQLite
			auto *buffer = static_cast<unsigned char *>(sqlite3_malloc64(image.size()));
			if(!buffer) errors::throw_sqlite_error(SQLITE_NOMEM, "deserialize");
			if(!image.empty()) std::memcpy(buffer, image.data(), image.size());
			if(int rc = sqlite3_deserialize(_db.get(), schema_name.c_str(), buffer,
			                                static_cast<sqlite3_int64>(image.size()),
			                                static_cast<sqlite3_int64>(image.size()),
			                                SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE))
				errors::throw_sqlite_error(rc, "deserialize", sqlite3_errmsg(_db.get()));
		}

		// Executes a parameterless statement and returns the affected row count.
		sqlite3_int64 execute(str_ref sql) {
			(*this << sql).execute();
			return rows_modified();
		}

		// Same as operator<< / execute, but reports failures through
		// execution_result instead of exceptions, for callers that prefer
		// value-based error handling.
		template<typename... Args>
			requires (bindable<Args> && ...)
		execution_result try_execute(str_ref sql, Args&&... args) noexcept {
			try {
				auto statement = *this << sql;
				((statement << std::forward<Args>(args)), ...);
				statement.execute();
				return execution_result{rows_modified(), SQLITE_OK, {}};
			} catch(const sqlite_exception &error) {
				return failed_execution(error.get_extended_code(), error.what());
			} catch(const std::exception &error) {
				return failed_execution(SQLITE_ERROR, error.what());
			} catch(...) {
				return failed_execution(SQLITE_ERROR, "unknown error");
			}
		}

		// Binds one parameter tuple per range element and executes the same
		// prepared statement for every row: one compile, N executions.
		template<typename Range>
			requires std::ranges::input_range<Range> &&
			         detail::tuple_like<std::ranges::range_value_t<Range>>
		sqlite3_int64 execute_many(str_ref sql, const Range& rows) {
			auto borrowed = _statements->acquire(sql);
			detail::statement_slot *slot = borrowed.first;
			std::unique_ptr<detail::statement_slot> owned;
			if(!borrowed.second) owned.reset(slot);
			sqlite3_stmt *stmt = slot ? slot->stmt.get() : nullptr;
			sqlite3_int64 executed = 0;
			try {
				if(stmt) {
					// Every row rebinds the same leading parameters; anything
					// beyond the tuple is bound to NULL once, so values left by an
					// earlier user of the cached statement never leak in.
					constexpr int width = static_cast<int>(
						std::tuple_size_v<std::remove_cvref_t<std::ranges::range_value_t<Range>>>);
					for(int index = width + 1, count = sqlite3_bind_parameter_count(stmt);
					    index <= count; ++index)
						sqlite3_bind_null(stmt, index);
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
					}
				}
			} catch(...) {
				if(!owned) _statements->release(slot);
				throw;
			}
			// Every row ended with a reset, so release has nothing left to do.
			if(!owned) _statements->release(slot, false);
			return executed;
		}

	private:
		// Builds a failure without letting an allocation failure escape the
		// noexcept try_execute: the code alone still reports the failure.
		static execution_result failed_execution(int code, const char *message) noexcept {
			execution_result result;
			result.code = code == SQLITE_OK ? SQLITE_ERROR : code;
			try {
				result.error = message;
			} catch(...) { // NOLINT(bugprone-empty-catch)
				// Out of memory: keep the code, drop the message.
			}
			return result;
		}

		// Every trampoline is noexcept and catches everything: an exception must
		// never unwind through SQLite's C frames.
		static int progress_trampoline(void *context) noexcept {
			auto *state = static_cast<connection_state *>(context);
			if(!state || !state->progress) return 0;
			try {
				return state->progress();
			} catch(...) {
				return 1; // a failing handler interrupts the statement
			}
		}

		static void update_trampoline(void *context, int action, const char *database_name,
		                              const char *table_name, sqlite3_int64 rowid) noexcept {
			auto *state = static_cast<connection_state *>(context);
			if(!state || !state->update) return;
			try {
				state->update(action,
				              database_name ? std::string_view(database_name) : std::string_view(),
				              table_name ? std::string_view(table_name) : std::string_view(), rowid);
			} catch(...) { // NOLINT(bugprone-empty-catch)
				// The row change has already happened; there is nothing to undo.
			}
		}

		static int commit_trampoline(void *context) noexcept {
			auto *state = static_cast<connection_state *>(context);
			if(!state || !state->commit) return 0;
			try {
				return state->commit();
			} catch(...) {
				return 1; // a failing handler turns the commit into a rollback
			}
		}

		static void rollback_trampoline(void *context) noexcept {
			auto *state = static_cast<connection_state *>(context);
			if(!state || !state->rollback) return;
			try {
				state->rollback();
			} catch(...) { // NOLINT(bugprone-empty-catch)
				// The rollback has already happened; there is nothing to undo.
			}
		}

	public:
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

		// Registers a SQL window function: step adds a row to the frame,
		// inverse removes one, value reports the current frame value and final
		// flushes at the end. All four take the shared state as their first
		// argument.
		template <typename StepFunction, typename InverseFunction,
		          typename ValueFunction, typename FinalFunction>
		void define_window(const std::string &name,
		                   StepFunction&& step, InverseFunction&& inverse,
		                   ValueFunction&& value, FinalFunction&& final,
		                   function_flags flags = function_flags::none) {
			typedef utility::function_traits<StepFunction> traits;
			using ContextType = typename std::remove_reference<
					typename traits::template argument<0>>::type;
			using Functions = std::tuple<
					typename std::remove_reference<StepFunction>::type,
					typename std::remove_reference<InverseFunction>::type,
					typename std::remove_reference<ValueFunction>::type,
					typename std::remove_reference<FinalFunction>::type>;
			static constexpr std::size_t arity = traits::arity - 1;

			auto funcs = new Functions(std::forward<StepFunction>(step),
			                           std::forward<InverseFunction>(inverse),
			                           std::forward<ValueFunction>(value),
			                           std::forward<FinalFunction>(final));
			if(int result = sqlite3_create_window_function(
					_db.get(), name.c_str(), static_cast<int>(arity),
					SQLITE_UTF8 | static_cast<int>(flags), funcs,
					sql_function_binder::window_step<ContextType, arity, Functions>,
					sql_function_binder::window_final<ContextType, Functions>,
					sql_function_binder::window_value<ContextType, Functions>,
					sql_function_binder::window_inverse<ContextType, arity, Functions>,
					[](void *ptr) { delete static_cast<Functions *>(ptr); })) {
				delete funcs;
				errors::throw_sqlite_error(result, {}, sqlite3_errmsg(_db.get()));
			}
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

} // namespace sqlite
