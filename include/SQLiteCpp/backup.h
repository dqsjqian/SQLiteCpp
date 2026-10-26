#pragma once

// Online backup between two connections using the sqlite3_backup_* API.

#include <memory>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

#include "types.h"
#include "errors.h"
#include "database.h"

namespace sqlite {

	// Copies a database into another connection, page by page. The handle is
	// released on destruction, so an abandoned backup never leaks.
	class backup {
	public:
		backup(database &source, database &target,
		       str_ref source_schema = "main", str_ref target_schema = "main")
			: _source(source.connection()), _target(target.connection()) {
			// SQLite wants NUL-terminated names; a string_view does not
			// guarantee one.
			const std::string source_name(source_schema);
			const std::string target_name(target_schema);
			_handle = sqlite3_backup_init(_target.get(), target_name.c_str(),
			                              _source.get(), source_name.c_str());
			if(!_handle)
				errors::throw_sqlite_error(sqlite3_extended_errcode(_target.get()), {},
				                           sqlite3_errmsg(_target.get()));
		}

		backup(const backup &) = delete;
		backup &operator=(const backup &) = delete;

		backup(backup &&other) noexcept:
			_source(std::move(other._source)),
			_target(std::move(other._target)),
			_handle(other._handle),
			_finished(other._finished) {
			other._handle = nullptr;
			other._finished = true;
		}

		~backup() {
			if(_handle) sqlite3_backup_finish(_handle);
		}

		// Copies up to `pages` pages (all remaining when negative). Returns
		// SQLITE_OK while more work remains, SQLITE_DONE when the copy is
		// complete, and SQLITE_BUSY / SQLITE_LOCKED when the caller should
		// retry. Other codes throw.
		int step(int pages = -1) {
			if(!_handle) throw std::logic_error("sqlite::backup: already finished");
			const int rc = sqlite3_backup_step(_handle, pages);
			if(rc == SQLITE_DONE)
				_finished = true;
			else if(rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
				errors::throw_sqlite_error(rc, {}, sqlite3_errmsg(_target.get()));
			return rc;
		}

		// Copies everything in one call, waiting out SQLITE_BUSY / LOCKED.
		void run() {
			while(true) {
				const int rc = step(-1);
				if(rc == SQLITE_DONE) break;
				if(rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
					errors::throw_sqlite_error(rc, {}, sqlite3_errmsg(_target.get()));
			}
		}

		bool done() const { return _finished; }
		int remaining() const { return _handle ? sqlite3_backup_remaining(_handle) : 0; }
		int page_count() const { return _handle ? sqlite3_backup_pagecount(_handle) : 0; }

		// Releases the handle and reports any error the copy left behind.
		void finish() {
			if(!_handle) return;
			const int rc = sqlite3_backup_finish(_handle);
			_handle = nullptr;
			_finished = true;
			if(rc != SQLITE_OK)
				errors::throw_sqlite_error(rc, {}, sqlite3_errmsg(_target.get()));
		}

	private:
		std::shared_ptr<sqlite3> _source;
		std::shared_ptr<sqlite3> _target;
		sqlite3_backup *_handle = nullptr;
		bool _finished = false;
	};

} // namespace sqlite
