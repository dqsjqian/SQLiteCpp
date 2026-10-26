#pragma once

#include <cstring>
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
			unsigned char *data = sqlite3_serialize(_db.get(), schema.data(), &size, 0);
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
			auto *buffer = static_cast<unsigned char *>(sqlite3_malloc64(image.size()));
			if(!buffer) errors::throw_sqlite_error(SQLITE_NOMEM, "deserialize");
			if(!image.empty()) std::memcpy(buffer, image.data(), image.size());
			if(int rc = sqlite3_deserialize(_db.get(), schema.data(), buffer,
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
				return execution_result{0, error.get_extended_code(), error.what()};
			} catch(const std::exception &error) {
				return execution_result{0, SQLITE_ERROR, error.what()};
			} catch(...) {
				return execution_result{0, SQLITE_ERROR, "unknown error"};
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

} // namespace sqlite
