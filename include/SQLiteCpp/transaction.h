#pragma once

#include <memory>
#include <stdexcept>
#include <sqlite3.h>
#include "fwd.h"
#include "types.h"
#include "errors.h"
#include "database.h"

namespace sqlite {

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

	// Nested transaction point. Rolls back to the savepoint on destruction
	// unless commit() was called. Names must be plain identifiers.
	class savepoint {
	public:
		savepoint(database &db, str_ref name) : _db(db.connection()), _name(name) {
			if(_name.empty() || _name.find_first_not_of(
					"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
				!= std::string::npos)
				throw std::invalid_argument("sqlite::savepoint: name must be an identifier");
			execute("SAVEPOINT ", _name);
			_active = true;
		}

		savepoint(const savepoint &) = delete;
		savepoint &operator=(const savepoint &) = delete;

		savepoint(savepoint &&other) noexcept:
			_db(std::move(other._db)), _name(std::move(other._name)), _active(other._active) {
			other._active = false;
		}

		~savepoint() noexcept {
			if(_active) {
				try {
					rollback();
				} catch(...) {
				}
			}
		}

		bool active() const { return _active; }

		// RELEASE: keeps the changes and drops only this savepoint.
		void commit() {
			if(!_active) throw std::logic_error("sqlite::savepoint: no longer active");
			_active = false;
			execute("RELEASE ", _name);
		}

		// ROLLBACK TO + RELEASE: discards the changes made since the savepoint.
		void rollback() {
			if(!_active) throw std::logic_error("sqlite::savepoint: no longer active");
			_active = false;
			execute("ROLLBACK TO ", _name);
			execute("RELEASE ", _name);
		}

	private:
		void execute(const char *prefix, const std::string &suffix) {
			std::string sql(prefix);
			sql += suffix;
			char *errmsg = nullptr;
			const int rc = sqlite3_exec(_db.get(), sql.c_str(), nullptr, nullptr, &errmsg);
			if(rc != SQLITE_OK) {
				const std::string message = errmsg ? errmsg : sqlite3_errstr(rc);
				sqlite3_free(errmsg);
				errors::throw_sqlite_error(rc, sql, message.c_str());
			}
		}

		std::shared_ptr<sqlite3> _db;
		std::string _name;
		bool _active = false;
	};

} // namespace sqlite
